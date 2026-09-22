// Scenes task: script-facing scene instancing -- a field declared `Scene`
// holds a saved .cscene asset path (a plain string under the hood; the
// Inspector's asset-picker is cosmetic), and calling .instantiate() on it
// loads that scene, attaches it under the calling script's own scene root,
// and returns an Actor reference to the new instance. Exercised from BOTH
// an interpreted and a natively-compiled caller.
//
// No framework: asserts + a pass counter, run via CTest. Skips gracefully
// (not a failure) without a discoverable MSVC toolchain, matching every
// other Phase 3+ test's convention.

#include "editor/ScriptBuild.h"
#include "scene/Actor3D.h"
#include "scene/BuiltinComponents.h"
#include "scene/ComponentRegistry.h"
#include "scene/Scene.h"
#include "script/NativeClassRegistry.h"
#include "script/NativeScriptComponent.h"
#include "script/ScriptComponent.h"
#include "script/ScriptSystem.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace crate;
using namespace crate::script;
using namespace crate::editor::scriptbuild;
namespace fs = std::filesystem;

#ifndef CRATE_SCRIPTS_DIR
#define CRATE_SCRIPTS_DIR "."
#endif

static int g_checks = 0;
#define CHECK(cond)                                                                   \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            return 1;                                                                \
        }                                                                           \
    } while (0)

static Value fieldOf(Component* c, const std::string& name) {
    if (auto* sc = dynamic_cast<ScriptComponent*>(c))
        return sc->object()->fields.at(name);
    if (auto* nc = dynamic_cast<NativeScriptComponent*>(c)) {
        auto view = nc->ensureNativeView();
        for (size_t i = 0; i < view.classInfo->fieldCount; ++i)
            if (view.classInfo->fields[i].name == name)
                return view.classInfo->fields[i].get(view.instance);
        std::fprintf(stderr, "fieldOf: native field '%s' not found\n", name.c_str());
        std::abort();
    }
    std::fprintf(stderr, "fieldOf: neither ScriptComponent nor NativeScriptComponent\n");
    std::abort();
}

int main() {
    registerBuiltinComponents(); // normally done by EditorApp startup

    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    if (!ToolchainEnv::get().available()) {
        std::printf("(no MSVC toolchain found -- scene_instantiate_script_tests skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    // A tiny standalone scene to instantiate: one actor with a Light.
    const std::string leafPath = "_ssi_leaf_test.cscene";
    {
        Scene leaf("Leaf");
        auto lamp = std::make_unique<Actor3D>("Lamp");
        lamp->addComponent(std::make_unique<LightComponent>());
        leaf.add(std::move(lamp));
        std::string err;
        CHECK(leaf.save(leafPath, &err));
    }

    struct Combo {
        const char* suffix;
        bool native;
    };
    static const Combo kCombos[] = {{"I", false}, {"N", true}};

    std::vector<std::string> paths;
    for (const auto& c : kCombos) {
        std::string src = "class SsiSpawner" + std::string(c.suffix) + " : Actor3D {\n"
                          "    Scene toSpawn;\n"
                          "    var spawnedName = \"\";\n"
                          "    var spawnedChildName = \"\";\n"
                          "    var spawnedIsNull = true;\n"
                          "    func start() {\n"
                          "        var inst = toSpawn.instantiate();\n"
                          "        spawnedIsNull = (inst == null);\n"
                          "        if (!spawnedIsNull) {\n"
                          "            spawnedName = inst.name;\n"
                          "        }\n"
                          "    }\n"
                          "}\n";
        std::string p = dir + "/SsiSpawner" + c.suffix + ".cscript";
        {
            std::ofstream o(p, std::ios::binary);
            o << src;
        }
        paths.push_back(p);
    }
    sys.reload();
    for (const auto& c : kCombos)
        CHECK(sys.file(std::string("SsiSpawner") + c.suffix) != nullptr);

    const std::string ns = "SsiSpawnerNativeNs";
    struct Cleanup {
        ScriptSystem* sys;
        std::vector<std::string> paths;
        std::string scriptsDir, ns, leafPath;
        ~Cleanup() {
            NativeClassRegistry::get().unloadAll();
            for (const auto& path : paths)
                std::remove(path.c_str());
            std::remove(leafPath.c_str());
            sys->reload();
            std::error_code ec;
            fs::remove_all(scriptsDir + "/.crate_build/gen/" + ns, ec);
            std::error_code ec2;
            for (const auto& entry : fs::directory_iterator(scriptsDir + "/.crate_build/bin", ec2)) {
                if (ec2)
                    break;
                if (entry.path().filename().string().rfind(ns + "_v", 0) == 0)
                    fs::remove(entry.path(), ec);
            }
            const std::string manifestPath = scriptsDir + "/.crate_build/manifest.tsv";
            std::ifstream in(manifestPath, std::ios::binary);
            if (in) {
                std::string kept, line;
                while (std::getline(in, line))
                    if (line.rfind(ns + "\t", 0) != 0)
                        kept += line + "\n";
                in.close();
                std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
                out << kept;
            }
        }
    } cleanup{&sys, paths, dir, ns, leafPath};

    sys.setNamespace("SsiSpawnerN", ns);
    BuildResult build = buildNamespace(ns, dir);
    if (!build.ok)
        std::printf("  buildNamespace error: %s\n", build.error.c_str());
    CHECK(build.ok);
    CHECK(NativeClassRegistry::get().loadNamespace(ns, build.dllPath, {"SsiSpawnerN"}));

    for (const auto& c : kCombos) {
        Scene scene(std::string("ssi-parity-") + c.suffix);
        auto* set = scene.add(std::make_unique<Actor3D>("Set")); // gives the scene a real root chain
        Actor* spawnerActor = scene.add(std::make_unique<Actor3D>("Spawner"), set);
        auto comp = ComponentRegistry::get().create(std::string("SsiSpawner") + c.suffix);
        CHECK(comp != nullptr);
        CHECK((dynamic_cast<NativeScriptComponent*>(comp.get()) != nullptr) == c.native);
        Component* spawner = spawnerActor->addComponent(std::move(comp));

        std::shared_ptr<ScriptObject> obj = c.native
                                                ? dynamic_cast<NativeScriptComponent*>(spawner)->object()
                                                : dynamic_cast<ScriptComponent*>(spawner)->object();
        CHECK(obj != nullptr);
        obj->fields["toSpawn"] = Value::Str(leafPath); // Inspector drag-drop would set this the same way

        int before = scene.actorCount();
        scene.startPlay(); // runs start(): toSpawn.instantiate()

        const std::string expectedName = fs::path(leafPath).stem().string(); // instantiate()'s
                                                                              // default name = the
                                                                              // source FILE's stem,
                                                                              // not Scene::name()
        CHECK(fieldOf(spawner, "spawnedIsNull").b == false);
        CHECK(fieldOf(spawner, "spawnedName").str() == expectedName);
        int after = scene.actorCount();
        CHECK(after == before + 2); // the instance root + its own child ("Lamp")

        // Confirm it's really in the tree, not just field-visible: instantiate()
        // anchors at sceneRootOf(callerOwner), i.e. the SCENE's own top-level
        // root (a sibling of "Set", not a child of it -- walks all the way up,
        // matching get_root()'s identical anchor point).
        (void)set;
        Actor* found = nullptr;
        for (const auto& ch : scene.root().children())
            if (ch->name() == expectedName)
                found = ch.get();
        CHECK(found != nullptr);
        CHECK(found->isInstanceRoot());
        CHECK(found->instanceSource() == leafPath);
        CHECK(found->children().size() == 1);
        CHECK(found->children()[0]->name() == "Lamp");
        CHECK(found->children()[0]->getComponent<LightComponent>() != nullptr);
        std::printf("ok  combo %s (%s caller): Scene-field.instantiate() spawned '%s' into the "
                    "live tree with its Lamp child intact\n",
                    c.suffix, c.native ? "native" : "interpreted", expectedName.c_str());
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
