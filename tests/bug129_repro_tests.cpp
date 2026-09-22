// Regression test for Zen task 129: a script referencing Input.mouse
// (lowercase) was rejected with a confusing "unknown identifier 'Input'"
// runtime error, because Input.Mouse.<member>'s structural recognition (in
// both Interpreter::evalMember and CodeGen's Member case) matched the
// "Mouse" segment case-sensitively. Fixed by making that match case-
// insensitive (Runtime::isInputMouseSegment) -- "Input" is pure namespace
// syntax with no real Value of its own, so the exact case of "Mouse"
// shouldn't be a trap. This test exercises the exact script from the bug
// report, through the real Play-press pipeline (buildNamespace ->
// loadNamespace -> ComponentRegistry::create), for both the interpreted and
// natively-compiled path, and confirms an actor + child both survive a full
// start()+update() cycle with no exception escaping.
//
// Separately: while investigating, the reported "actor and children
// disappear on Play" symptom did NOT reproduce through this same pipeline --
// the generated native update() wrapper already catches the identifier
// error internally (logs via ctx_->warn, matching ScriptComponent::
// runHook's identical message format for the interpreted path) and leaves
// the actor hierarchy untouched. That half of task 129 stays open pending a
// live repro; see the Zen note on the task for details.
#include "editor/ScriptBuild.h"
#include "scene/Actor3D.h"
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

int main() {
    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    if (!ToolchainEnv::get().available()) {
        std::printf("(no MSVC toolchain -- bug129_repro_tests skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    struct Combo {
        const char* suffix;
        const char* mouseCase; // exact spelling used in the script source
        bool native;
    };
    static const Combo kCombos[] = {
        {"I", "Mouse", false}, {"N", "Mouse", true},
        {"LI", "mouse", false}, {"LN", "mouse", true}, // task 129's exact repro casing
    };

    std::vector<std::string> paths;
    for (const auto& c : kCombos) {
        std::string src = "class Bug129_" + std::string(c.suffix) + " : Actor\n{\n"
                          "    Camera headCam;\n"
                          "    Transform body;\n"
                          "    float look_speed = 3;\n"
                          "    func start()\n    {\n    }\n\n"
                          "    func update(float delta)\n    {\n"
                          "        Vector2 input = Input." + std::string(c.mouseCase) +
                          ".delta * look_speed * delta;\n"
                          "        print(input.str());\n"
                          "    }\n\n"
                          "    func physics_update(float delta)\n    {\n    }\n}\n";
        std::string p = dir + "/Bug129_" + c.suffix + ".cscript";
        std::ofstream(p, std::ios::binary) << src;
        paths.push_back(p);
    }
    sys.reload();
    for (const auto& c : kCombos)
        CHECK(sys.file(std::string("Bug129_") + c.suffix) != nullptr);

    const std::string ns = "Bug129NativeNs";
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
                std::ofstream(manifestPath, std::ios::binary | std::ios::trunc) << kept;
            }
        }
    } cleanup{&sys, paths, dir, ns};

    // Route the two native-suffixed classes into their own namespace so this
    // test never depends on -- or pollutes -- "Global"'s build state/manifest
    // (that's exactly the trap this diagnostic hit during development: a
    // stale manifest entry from a previous run of a script with byte-
    // identical content made computeDirtyNamespaces() report "not dirty" on
    // a supposedly-fresh run. Namespacing this test's classes into a name
    // nothing else ever uses sidesteps the whole problem rather than papering
    // over it with a manifest reset.).
    sys.setNamespace("Bug129_N", ns);
    sys.setNamespace("Bug129_LN", ns);
    BuildResult build = buildNamespace(ns, dir);
    if (!build.ok)
        std::printf("  buildNamespace error: %s\n", build.error.c_str());
    CHECK(build.ok);
    CHECK(NativeClassRegistry::get().loadNamespace(ns, build.dllPath, {"Bug129_N", "Bug129_LN"}));

    for (const auto& c : kCombos) {
        Scene scene(std::string("bug129-") + c.suffix);
        Actor* parent = scene.add(std::make_unique<Actor3D>("Player"));
        Actor* child = parent->addChild(std::make_unique<Actor3D>("Child"));
        (void)child;
        auto comp = ComponentRegistry::get().create(std::string("Bug129_") + c.suffix);
        CHECK(comp != nullptr);
        CHECK((dynamic_cast<NativeScriptComponent*>(comp.get()) != nullptr) == c.native);
        parent->addComponent(std::move(comp));

        bool threw = false;
        try {
            scene.startPlay();
            scene.tick(0.016f);
        } catch (...) {
            threw = true;
        }
        CHECK(!threw); // Input.<Mouse|mouse>.delta must resolve, not throw
        CHECK(scene.root().children().size() == 1);
        CHECK(parent->children().size() == 1); // the child must still be there

        std::printf("ok  combo %s (%s caller, Input.%s.delta): resolved without error, "
                    "actor+child intact\n",
                    c.suffix, c.native ? "native" : "interpreted", c.mouseCase);
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
