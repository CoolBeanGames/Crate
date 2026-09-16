// Phase 9c end-to-end parity test: Camera.main read + Camera.main = ...
// assignment, exercised from BOTH an interpreted and a natively-compiled
// caller, asserting identical observable results (which camera each caller
// reads as "main", and that assigning a different one actually switches the
// scene's active camera for everyone -- verified both through the script's
// own field AND directly against the real CameraComponent instances). See
// transpiration.txt, "Transplation" Phase 9c.
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

// Same rationale as get_component_parity_tests.cpp's fieldOf: for a native
// component, read straight through the CompiledClassInfo accessor (what
// script code itself sees), not the Inspector-display mirror, which is only
// refreshed around this component's OWN hook calls.
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
    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    if (!ToolchainEnv::get().available()) {
        std::printf("(no MSVC toolchain found -- camera_main_parity_tests skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    struct Combo {
        const char* suffix;
        bool native;
    };
    static const Combo kCombos[] = {{"I", false}, {"N", true}};

    std::vector<std::string> paths;
    for (const auto& c : kCombos) {
        std::string src = "class GcCamReader" + std::string(c.suffix) + " : Actor3D {\n"
                          "    var otherCamActor = null;\n"
                          "    var beforeFov = 0.0;\n"
                          "    var afterFov = 0.0;\n"
                          "    func start() {\n"
                          "        var cam = Camera.main;\n"
                          "        beforeFov = cam.fov;\n"
                          "        var other = this.otherCamActor.get_component(type_of(Camera));\n"
                          "        Camera.main = other;\n"
                          "        var cam2 = Camera.main;\n"
                          "        afterFov = cam2.fov;\n"
                          "    }\n"
                          "}\n";
        std::string p = dir + "/GcCamReader" + c.suffix + ".cscript";
        {
            std::ofstream o(p, std::ios::binary);
            o << src;
        }
        paths.push_back(p);
    }
    sys.reload();
    for (const auto& c : kCombos)
        CHECK(sys.file(std::string("GcCamReader") + c.suffix) != nullptr);

    const std::string ns = "GcCamReaderNativeNs";
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

    sys.setNamespace("GcCamReaderN", ns);
    BuildResult build = buildNamespace(ns, dir);
    if (!build.ok)
        std::printf("  buildNamespace error: %s\n", build.error.c_str());
    CHECK(build.ok);
    CHECK(NativeClassRegistry::get().loadNamespace(ns, build.dllPath, {"GcCamReaderN"}));

    for (const auto& c : kCombos) {
        Scene scene(std::string("cam-parity-") + c.suffix);
        Actor* camAActor = scene.add(std::make_unique<Actor3D>("CamA"));
        Actor* camBActor = scene.add(std::make_unique<Actor3D>("CamB"));
        auto* camA =
            static_cast<CameraComponent*>(camAActor->addComponent(std::make_unique<CameraComponent>()));
        auto* camB =
            static_cast<CameraComponent*>(camBActor->addComponent(std::make_unique<CameraComponent>()));
        camA->fovY = 11.0f;
        camA->enabled = true;
        camB->fovY = 22.0f;
        camB->enabled = false;

        Actor* readerActor = scene.add(std::make_unique<Actor3D>("Reader"));
        auto comp = ComponentRegistry::get().create(std::string("GcCamReader") + c.suffix);
        CHECK(comp != nullptr);
        CHECK((dynamic_cast<NativeScriptComponent*>(comp.get()) != nullptr) == c.native);
        Component* reader = readerActor->addComponent(std::move(comp));

        // Point the reader at CamB (an Actor reference, exactly as the
        // Inspector's drag-drop actor picker would produce) BEFORE Play --
        // this is pushed into the native instance (if any) by
        // NativeScriptComponent::start()'s own pushFieldsToNative() call,
        // which always runs before the generated start() body executes.
        std::shared_ptr<ScriptObject> obj = c.native ? dynamic_cast<NativeScriptComponent*>(reader)->object()
                                                     : dynamic_cast<ScriptComponent*>(reader)->object();
        CHECK(obj != nullptr);
        obj->fields["otherCamActor"] = Value::ActorRef(camBActor);

        scene.startPlay(); // runs start(): reads Camera.main (A), switches to B, re-reads

        CHECK(fieldOf(reader, "beforeFov").num() == 11.0);
        CHECK(fieldOf(reader, "afterFov").num() == 22.0);
        CHECK(camA->enabled == false); // activateMainCamera disabled the old main
        CHECK(camB->enabled == true);  // ...and enabled the new one
        std::printf("ok  combo %s (%s caller): Camera.main read A (fov=11), assigned B, re-read "
                    "found B (fov=22); camA disabled, camB enabled\n",
                    c.suffix, c.native ? "native" : "interpreted");
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
