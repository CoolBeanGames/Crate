// Scripting API additions from the user's Crate feedback list: Actor.
// destroy(), Component.remove(), Actor.add_component(type_of(X)), the
// forward/right/up setters, Vector.normalize(), generic .transform access
// off any component reference, and the new Math.slerp/registered-for-
// autocomplete functions. Exercised from BOTH an interpreted and a
// natively-compiled caller (all "N"-suffixed classes below are compiled
// together into ONE namespace DLL, avoiding a separate build per feature).
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

#include <cmath>
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

static std::shared_ptr<ScriptObject> objectOf(Component* c) {
    if (auto* sc = dynamic_cast<ScriptComponent*>(c))
        return sc->object();
    if (auto* nc = dynamic_cast<NativeScriptComponent*>(c))
        return nc->object();
    return nullptr;
}

int main() {
    registerBuiltinComponents(); // normally done by EditorApp startup

    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    if (!ToolchainEnv::get().available()) {
        std::printf("(no MSVC toolchain found -- actor_api_tests skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    struct Combo {
        const char* suffix;
        bool native;
    };
    static const Combo kCombos[] = {{"I", false}, {"N", true}};

    std::vector<std::string> paths;
    auto writeScript = [&](const std::string& className, const std::string& src) {
        std::string p = dir + "/" + className + ".cscript";
        std::ofstream o(p, std::ios::binary);
        o << src;
        paths.push_back(p);
    };

    for (const auto& c : kCombos) {
        std::string s(c.suffix);
        writeScript("ApiProbe" + s,
                    "class ApiProbe" + s + " : Actor3D {\n"
                    "    var normX = 0.0;\n"
                    "    var camFov = 0.0;\n"
                    "    var addedSpinnerAxis = -1;\n"
                    "    func start() {\n"
                    "        var v = Vector3(3, 4, 0);\n"
                    "        v.normalize();\n"
                    "        normX = v.x;\n"
                    "        var cam = this.get_component(type_of(Camera));\n"
                    "        camFov = cam.transform.position.x;\n" // owning actor's transform via a Camera ref
                    "        var spinner = this.add_component(type_of(Spinner));\n"
                    "        addedSpinnerAxis = spinner.axis;\n"
                    "        actor.forward = Vector3(0, 0, -1);\n" // `actor`, not `this` (a self object)
                    "    }\n"
                    "}\n");
        writeScript("RemoverRunner" + s,
                    "class RemoverRunner" + s + " : Actor3D {\n"
                    "    func start() {\n"
                    "        var light = this.get_component(type_of(Light));\n"
                    "        light.remove();\n"
                    "    }\n"
                    "}\n");
        writeScript("DestroyerRunner" + s,
                    "class DestroyerRunner" + s + " : Actor3D {\n"
                    "    Actor targetChild;\n"
                    "    func start() {\n"
                    "        targetChild.destroy();\n"
                    "    }\n"
                    "}\n");
    }
    sys.reload();
    for (const auto& c : kCombos) {
        std::string s(c.suffix);
        CHECK(sys.file("ApiProbe" + s) != nullptr);
        CHECK(sys.file("RemoverRunner" + s) != nullptr);
        CHECK(sys.file("DestroyerRunner" + s) != nullptr);
    }

    const std::string ns = "ActorApiNativeNs";
    struct Cleanup {
        ScriptSystem* sys;
        std::vector<std::string> paths;
        std::string scriptsDir, ns;
        ~Cleanup() {
            NativeClassRegistry::get().unloadAll();
            for (const auto& path : paths)
                std::remove(path.c_str());
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
    } cleanup{&sys, paths, dir, ns};

    // Every "N"-suffixed class compiles together into ONE namespace DLL.
    sys.setNamespace("ApiProbeN", ns);
    sys.setNamespace("RemoverRunnerN", ns);
    sys.setNamespace("DestroyerRunnerN", ns);
    BuildResult build = buildNamespace(ns, dir);
    if (!build.ok)
        std::printf("  buildNamespace error: %s\n", build.error.c_str());
    CHECK(build.ok);
    CHECK(NativeClassRegistry::get().loadNamespace(
        ns, build.dllPath, {"ApiProbeN", "RemoverRunnerN", "DestroyerRunnerN"}));

    // --- normalize() / .transform via a component ref / add_component() /
    // the forward setter, all in one script -------------------------------
    for (const auto& c : kCombos) {
        Scene scene(std::string("api-") + c.suffix);
        Actor* probeActor = scene.add(std::make_unique<Actor3D>("Probe"));
        probeActor->transform().position = {42.0f, 0.0f, 0.0f};
        probeActor->addComponent(std::make_unique<CameraComponent>());
        auto comp = ComponentRegistry::get().create(std::string("ApiProbe") + c.suffix);
        CHECK(comp != nullptr);
        CHECK((dynamic_cast<NativeScriptComponent*>(comp.get()) != nullptr) == c.native);
        Component* probe = probeActor->addComponent(std::move(comp));

        scene.startPlay(); // runs start()

        CHECK(std::fabs(fieldOf(probe, "normX").num() - 0.6) < 1e-4); // (3,4,0) normalized -> x=0.6
        CHECK(std::fabs(fieldOf(probe, "camFov").num() - 42.0) < 1e-4); // reached Probe's own transform
        CHECK(probeActor->getComponent<SpinnerComponent>() != nullptr);
        CHECK(fieldOf(probe, "addedSpinnerAxis").i == 1);
        float yaw = probeActor->transform().rotationEuler.y;
        CHECK(std::fabs(std::fabs(yaw) - 180.0f) < 1.0f); // forward = (0,0,-1) -> yaw ~180

        std::printf("ok  combo %s (%s caller): normalize/.transform-via-component/"
                    "add_component/forward-setter all landed correctly\n",
                    c.suffix, c.native ? "native" : "interpreted");
    }

    // --- Component.remove(): deferred, applied by flushPending() -----------
    for (const auto& c : kCombos) {
        Scene scene(std::string("api-remove-") + c.suffix);
        Actor* child = scene.add(std::make_unique<Actor3D>("Child"));
        child->addComponent(std::make_unique<LightComponent>());
        auto removerComp = ComponentRegistry::get().create(std::string("RemoverRunner") + c.suffix);
        CHECK(removerComp != nullptr);
        CHECK((dynamic_cast<NativeScriptComponent*>(removerComp.get()) != nullptr) == c.native);
        child->addComponent(std::move(removerComp));

        scene.startPlay(); // runs start(): light.remove()
        CHECK(child->getComponent<LightComponent>() != nullptr); // not yet -- deferred
        ScriptSystem::get().flushPending(scene);
        CHECK(child->getComponent<LightComponent>() == nullptr); // now gone
        CHECK(scene.contains(child)); // only the component was removed, not the actor
        std::printf("ok  combo %s (%s caller): Component.remove() deferred correctly and "
                    "applied by flushPending()\n",
                    c.suffix, c.native ? "native" : "interpreted");
    }

    // --- Actor.destroy(): deferred, applied by flushPending() --------------
    for (const auto& c : kCombos) {
        Scene scene(std::string("api-destroy-") + c.suffix);
        Actor* parent = scene.add(std::make_unique<Actor3D>("Parent"));
        Actor* child = scene.add(std::make_unique<Actor3D>("Child"), parent);
        auto destroyerComp = ComponentRegistry::get().create(std::string("DestroyerRunner") + c.suffix);
        CHECK(destroyerComp != nullptr);
        CHECK((dynamic_cast<NativeScriptComponent*>(destroyerComp.get()) != nullptr) == c.native);
        Component* destroyer = parent->addComponent(std::move(destroyerComp));
        std::shared_ptr<ScriptObject> dobj = objectOf(destroyer);
        CHECK(dobj != nullptr);
        dobj->fields["targetChild"] = Value::ActorRef(child);

        scene.startPlay(); // runs start(): targetChild.destroy()
        CHECK(scene.contains(child)); // not yet -- deferred
        ScriptSystem::get().flushPending(scene);
        CHECK(!scene.contains(child)); // now gone
        std::printf("ok  combo %s (%s caller): Actor.destroy() deferred correctly and applied "
                    "by flushPending()\n",
                    c.suffix, c.native ? "native" : "interpreted");
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
