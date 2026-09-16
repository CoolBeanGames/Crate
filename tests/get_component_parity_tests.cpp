// Phase 9b end-to-end parity test: get_component() + a cross-object method
// call + cross-object field read/write, exercised across all FOUR
// combinations of interpreted/native on the CALLER axis (the object whose
// script calls this.get_component(...)) crossed with the TARGET axis (the
// sibling component it finds and then calls/reads/writes) -- proving
// get_component's uniform ctx_->getComponent()-based dispatch really is
// uniform, regardless of which side(s) happen to be compiled. See
// transpiration.txt, "Transplation" Phase 9b.
//
// No framework: asserts + a pass counter, run via CTest. Skips gracefully
// (not a failure) without a discoverable MSVC toolchain, matching every
// other Phase 3+ test's convention.

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

// Reads a field regardless of whether `c` is the interpreted or the
// compiled component kind. For a NativeScriptComponent this deliberately
// reads straight through its CompiledClassInfo accessor table -- i.e.
// exactly what script code calling t.value itself sees -- rather than
// nc->object()'s interpreted MIRROR, which is Phase 5's Inspector-display
// convenience and is only refreshed around THAT component's own hook
// calls: a cross-object write landing in it via get_component() (this
// test's whole point) is immediately visible to any other script that
// reads it, but is NOT reflected in the mirror until this component's own
// next hook runs, since nothing re-pulls it in between. That staleness
// window is expected/by-design (see NativeScriptComponent.h's big
// comment), not a bug -- so the test reads the live value, matching what
// script-to-script access actually sees.
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
        std::printf("(no MSVC toolchain found -- get_component_parity_tests skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    // One Holder+Target class pair per combination, so each combination's
    // compiled-ness (a per-class-name namespace assignment) is independent
    // of the others -- see the big comment above.
    struct Combo {
        const char* suffix;
        bool holderNative;
        bool targetNative;
    };
    static const Combo kCombos[] = {
        {"II", false, false},
        {"NI", true, false},
        {"IN", false, true},
        {"NN", true, true},
    };

    // One class per file (`<className>.cscript`), matching how
    // ScriptSystem::reload() discovers classes -- unlike
    // native_reflection_tests.cpp's single-class scenario, this test needs
    // 8 (4 combos x Holder/Target).
    std::vector<std::string> paths;
    for (const auto& c : kCombos) {
        std::string targetSrc = "class GcTarget" + std::string(c.suffix) + " : Actor3D {\n"
                                "    var value = 1;\n"
                                "    func bump(int by) {\n"
                                "        value = value + by;\n"
                                "        return value;\n"
                                "    }\n"
                                "}\n";
        std::string holderSrc = "class GcHolder" + std::string(c.suffix) + " : Actor3D {\n"
                                "    var result = 0;\n"
                                "    func start() {\n"
                                "        var t = this.get_component(type_of(GcTarget" +
                                std::string(c.suffix) +
                                "));\n"
                                "        t.value = t.value + 10;\n"
                                "        result = t.bump(5);\n"
                                "    }\n"
                                "}\n";
        std::string targetPath = dir + "/GcTarget" + c.suffix + ".cscript";
        std::string holderPath = dir + "/GcHolder" + c.suffix + ".cscript";
        {
            std::ofstream o(targetPath, std::ios::binary);
            o << targetSrc;
        }
        {
            std::ofstream o(holderPath, std::ios::binary);
            o << holderSrc;
        }
        paths.push_back(targetPath);
        paths.push_back(holderPath);
    }
    sys.reload();
    for (const auto& c : kCombos) {
        CHECK(sys.file(std::string("GcHolder") + c.suffix) != nullptr);
        CHECK(sys.file(std::string("GcTarget") + c.suffix) != nullptr);
    }

    // Every namespace this test creates, so cleanup can unload/scrub all of
    // them regardless of which combos actually got built below.
    std::vector<std::string> namespaces = {"GcNsHolderOnly", "GcNsTargetOnly", "GcNsBoth"};

    struct Cleanup {
        ScriptSystem* sys;
        std::vector<std::string> paths;
        std::string scriptsDir;
        std::vector<std::string> namespaces;
        ~Cleanup() {
            NativeClassRegistry::get().unloadAll();
            for (const auto& path : paths)
                std::remove(path.c_str());
            sys->reload();
            for (const auto& ns : namespaces) {
                std::error_code ec;
                fs::remove_all(scriptsDir + "/.crate_build/gen/" + ns, ec);
                std::error_code ec2;
                for (const auto& entry :
                    fs::directory_iterator(scriptsDir + "/.crate_build/bin", ec2)) {
                    if (ec2)
                        break;
                    if (entry.path().filename().string().rfind(ns + "_v", 0) == 0)
                        fs::remove(entry.path(), ec);
                }
            }
            const std::string manifestPath = scriptsDir + "/.crate_build/manifest.tsv";
            std::ifstream in(manifestPath, std::ios::binary);
            if (in) {
                std::string kept, line;
                while (std::getline(in, line)) {
                    bool drop = false;
                    for (const auto& ns : namespaces)
                        if (line.rfind(ns + "\t", 0) == 0)
                            drop = true;
                    if (!drop)
                        kept += line + "\n";
                }
                in.close();
                std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
                out << kept;
            }
        }
    } cleanup{&sys, paths, dir, namespaces};

    // ---- assign namespaces + build (only the 3 combos that need it -- II
    // stays fully interpreted, never assigned a namespace) -----------------
    sys.setNamespace("GcHolderNI", "GcNsHolderOnly");
    sys.setNamespace("GcTargetIN", "GcNsTargetOnly");
    sys.setNamespace("GcHolderNN", "GcNsBoth");
    sys.setNamespace("GcTargetNN", "GcNsBoth");

    for (const auto& ns : namespaces) {
        BuildResult build = buildNamespace(ns, dir);
        if (!build.ok)
            std::printf("  buildNamespace(%s) error: %s\n", ns.c_str(), build.error.c_str());
        CHECK(build.ok);
        std::vector<std::string> classes;
        if (ns == "GcNsHolderOnly")
            classes = {"GcHolderNI"};
        else if (ns == "GcNsTargetOnly")
            classes = {"GcTargetIN"};
        else
            classes = {"GcHolderNN", "GcTargetNN"};
        CHECK(NativeClassRegistry::get().loadNamespace(ns, build.dllPath, classes));
    }

    // ---- run all 4 combos on one Scene, one frame of startPlay() each ----
    Scene scene("gc-parity");
    struct Live {
        const Combo* combo;
        Component* holder;
        Component* target;
    };
    std::vector<Live> live;
    for (const auto& c : kCombos) {
        Actor* a = scene.add(std::make_unique<Actor3D>(std::string("Subject_") + c.suffix));
        auto targetComp = ComponentRegistry::get().create(std::string("GcTarget") + c.suffix);
        CHECK(targetComp != nullptr);
        CHECK((dynamic_cast<NativeScriptComponent*>(targetComp.get()) != nullptr) == c.targetNative);
        Component* target = a->addComponent(std::move(targetComp));

        auto holderComp = ComponentRegistry::get().create(std::string("GcHolder") + c.suffix);
        CHECK(holderComp != nullptr);
        CHECK((dynamic_cast<NativeScriptComponent*>(holderComp.get()) != nullptr) == c.holderNative);
        Component* holder = a->addComponent(std::move(holderComp));

        live.push_back({&c, holder, target});
    }

    scene.startPlay(); // runs every start() once, including the get_component() calls

    for (const auto& l : live) {
        const Combo& c = *l.combo;
        Component* holder = l.holder;
        Component* target = l.target;

        // t.value started at 1, get_component() found it, "+= 10" -> 11,
        // then bump(5) -> 16 (both via the cross-object method call AND the
        // cross-object field read/write get_component() unlocks).
        CHECK(fieldOf(target, "value").i == 16);
        CHECK(fieldOf(holder, "result").i == 16); // bump()'s return value
        std::printf("ok  combo %s (holder %s, target %s): get_component + field write + method "
                    "call all landed on the right sibling (value=16, result=16)\n",
                    c.suffix, c.holderNative ? "native" : "interpreted",
                    c.targetNative ? "native" : "interpreted");
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
