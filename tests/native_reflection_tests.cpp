// THE most important test in the "Transplation" plan (transpiration.txt,
// Phase 5): runs IDENTICAL cScript source through both the tree-walking
// interpreter (ScriptComponent) and the natively-compiled path
// (NativeScriptComponent, via a real build through Phase 3/4's pipeline)
// and asserts byte-identical observable results. This is the primary
// correctness guarantee for "everything works exactly as it does now."
//
// Also specifically exercises Phase 5's own new behavior that has no
// interpreter equivalent to compare against: that editing a
// NativeScriptComponent's interpreted mirror (obj_->fields) -- exactly
// what the Inspector does while Play is running -- actually reaches the
// running native instance before its next tick, and that the native
// instance's resulting values are pulled back so the mirror (and thus the
// Inspector) reflects them afterward.
//
// Skips gracefully (not a failure) without a discoverable MSVC toolchain,
// matching every other Phase 3+ test's convention.
//
// No framework: asserts + a pass counter, run via CTest.

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

static bool valuesEqual(const Value& a, const Value& b) {
    if (a.t != b.t)
        return false;
    switch (a.t) {
        case Value::T::Null: return true;
        case Value::T::Bool: return a.b == b.b;
        case Value::T::Int: return a.i == b.i;
        case Value::T::Float: return a.f == b.f;
        case Value::T::Char:
        case Value::T::String: return a.s == b.s;
        default: return a.str() == b.str(); // sufficient for the field types this test uses
    }
}

int main() {
    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    if (!ToolchainEnv::get().available()) {
        std::printf("(no MSVC toolchain found -- native_reflection_tests skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    const std::string ns = "NrTestNamespace";
    const std::string p = dir + "/NrTestCounter.cscript";
    {
        std::ofstream o(p, std::ios::binary);
        // Deliberately exercises: a plain int field, a string field
        // mutated on a conditional branch, arithmetic, and an UNDECLARED
        // (auto-vivified) field -- every one of decision #4's parity
        // requirements in a single small script.
        o << "class NrTestCounter : Actor3D {\n"
             "    var n = 0;\n"
             "    var label = \"start\";\n"
             "    func update(float delta) {\n"
             "        n = n + 1;\n"
             "        if (n == 5) { label = \"five\"; }\n"
             "        doubled = n * 2;\n"
             "    }\n"
             "}\n";
    }
    sys.reload();
    CHECK(sys.file("NrTestCounter") != nullptr);

    struct Cleanup {
        ScriptSystem* sys;
        std::string p, scriptsDir, ns;
        ~Cleanup() {
            NativeClassRegistry::get().unloadAll();
            std::remove(p.c_str());
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
    } cleanup{&sys, p, dir, ns};

    // ---- (1) interpreted path: ComponentRegistry still resolves to
    // ScriptComponent BEFORE this class's namespace is ever compiled/loaded
    // (compile() always registers via registerComponent for every
    // non-static/non-abstract class, regardless of namespace -- Phase 1-4
    // never changed that). Run 8 frames, record obj_->fields. ----------
    ScriptObject interpretedFinal;
    {
        Scene scene("interp");
        Actor* a = scene.add(std::make_unique<Actor3D>("Subject"));
        auto comp = ComponentRegistry::get().create("NrTestCounter");
        CHECK(comp != nullptr);
        auto* sc = dynamic_cast<ScriptComponent*>(comp.get());
        CHECK(sc != nullptr); // confirms this really is the interpreted path, not already native
        a->addComponent(std::move(comp));

        scene.startPlay();
        for (int i = 0; i < 8; ++i)
            scene.tick(0.1f);

        auto obj = sc->object();
        CHECK(obj != nullptr);
        interpretedFinal.fields = obj->fields; // snapshot for comparison below
    }
    std::printf("ok  interpreted path ran: n=%lld label=%s doubled=%lld\n",
               (long long)interpretedFinal.fields.at("n").i, interpretedFinal.fields.at("label").s.c_str(),
               (long long)interpretedFinal.fields.at("doubled").i);
    CHECK(interpretedFinal.fields.at("n").i == 8);
    CHECK(interpretedFinal.fields.at("label").s == "five"); // n reached 5 partway through
    CHECK(interpretedFinal.fields.at("doubled").i == 16);

    // ---- (2) compiled path: build + load, same class name now resolves
    // to NativeScriptComponent via ComponentRegistry (replace=true). Run
    // the SAME 8 frames, compare field-by-field. -------------------------
    sys.setNamespace("NrTestCounter", ns);
    BuildResult build = buildNamespace(ns, dir);
    if (!build.ok)
        std::printf("  buildNamespace error: %s\n", build.error.c_str());
    CHECK(build.ok);
    CHECK(NativeClassRegistry::get().loadNamespace(ns, build.dllPath, {"NrTestCounter"}));

    {
        Scene scene("native");
        Actor* a = scene.add(std::make_unique<Actor3D>("Subject"));
        auto comp = ComponentRegistry::get().create("NrTestCounter");
        CHECK(comp != nullptr);
        auto* nc = dynamic_cast<NativeScriptComponent*>(comp.get());
        CHECK(nc != nullptr); // confirms this really is the compiled path now
        a->addComponent(std::move(comp));

        scene.startPlay();
        for (int i = 0; i < 8; ++i)
            scene.tick(0.1f);

        auto obj = nc->object();
        CHECK(obj != nullptr);

        CHECK(obj->fields.count("n") == 1);
        CHECK(obj->fields.count("label") == 1);
        CHECK(obj->fields.count("doubled") == 1);
        CHECK(valuesEqual(obj->fields.at("n"), interpretedFinal.fields.at("n")));
        CHECK(valuesEqual(obj->fields.at("label"), interpretedFinal.fields.at("label")));
        CHECK(valuesEqual(obj->fields.at("doubled"), interpretedFinal.fields.at("doubled")));
        std::printf("ok  compiled path matches interpreted path EXACTLY after 8 frames: n=%lld "
                    "label=%s doubled=%lld\n",
                    (long long)obj->fields.at("n").i, obj->fields.at("label").s.c_str(),
                    (long long)obj->fields.at("doubled").i);

        // ---- (3) Inspector-during-Play sync (Phase 5's own new behavior,
        // no interpreter equivalent to compare against -- verified
        // directly): mutate the interpreted mirror exactly as the
        // Inspector would, tick once more, and confirm the RUNNING NATIVE
        // CODE actually saw the edit (not just that the mirror itself
        // changed). ------------------------------------------------------
        obj->fields["n"] = Value::Int(100);
        scene.tick(0.1f); // push(100) -> native runs n=101, doubled=202 -> pull
        auto obj2 = nc->object();
        CHECK(obj2->fields.at("n").i == 101); // proves the push reached the native instance
        CHECK(obj2->fields.at("doubled").i == 202); // proves the pull reflects what native code did
        std::printf("ok  editing the interpreted mirror (simulating an Inspector edit) reaches the "
                    "running native code on the next tick, and the result is pulled back (n=101, "
                    "doubled=202)\n");
    }

    // ---- (4) unload cleanly (Scene above is already destroyed, so no
    // live native instance survives the DLL going away -- see
    // transpiration.txt's ordering-hazard note from Phase 4). Unloading
    // must restore the INTERPRETED registration, not just remove the class
    // from ComponentRegistry entirely -- otherwise the class would vanish
    // from the Add-Component menu after every Stop (Phase 7), which would
    // be a real, confusing regression from today's edit-time behavior. ---
    NativeClassRegistry::get().unloadNamespace(ns);
    CHECK(ComponentRegistry::get().has("NrTestCounter"));
    {
        auto comp = ComponentRegistry::get().create("NrTestCounter");
        CHECK(comp != nullptr);
        CHECK(dynamic_cast<ScriptComponent*>(comp.get()) != nullptr);
        CHECK(dynamic_cast<NativeScriptComponent*>(comp.get()) == nullptr);
    }
    std::printf("ok  unloadNamespace() restores the INTERPRETED registration (class stays addable, "
                "runs interpreted again -- matches edit-time behavior)\n");

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
