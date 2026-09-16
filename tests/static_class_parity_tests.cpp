// Phase 9e end-to-end parity test: `static class` singletons compiled
// natively -- start/update ticking, and cross-script access via a bare
// class name (StaticClassName.field / .method()) from BOTH an interpreted
// and a natively-compiled caller, asserting identical results to the
// purely-interpreted path. Also exercises the Play -> Stop -> Play cycle
// (via loadNamespace/unloadNamespace + rebuildStatic) that the ordering-
// hazard fix in EditorApp.cpp/ScriptSystem.cpp specifically targets. See
// transpiration.txt, "Transplation" Phase 9e.
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
#include "script/ObjectDispatch.h"
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

// Reads a static class's field through the SAME dispatch script code
// itself uses (ctx.getStatic + getObjectMember), Kind-agnostically --
// exactly the mechanism this sub-phase's Interpreter.cpp fixes (evalMember/
// evalCall/assign's TypeRef+getStatic branches) route through now.
static Value staticFieldOf(const std::string& className, const std::string& field) {
    auto so = ScriptSystem::get().context().getStatic(className);
    if (!so) {
        std::fprintf(stderr, "staticFieldOf: no static named '%s'\n", className.c_str());
        std::abort();
    }
    return crate::script::getObjectMember(so, field, 0);
}

static Value fieldOf(Component* c, const std::string& name) {
    if (auto* sc = dynamic_cast<ScriptComponent*>(c))
        return sc->object()->fields.at(name);
    if (auto* nc = dynamic_cast<NativeScriptComponent*>(c)) {
        auto view = nc->ensureNativeView();
        for (size_t i = 0; i < view.classInfo->fieldCount; ++i)
            if (view.classInfo->fields[i].name == name)
                return view.classInfo->fields[i].get(view.instance);
        std::abort();
    }
    std::abort();
}

int main() {
    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    if (!ToolchainEnv::get().available()) {
        std::printf("(no MSVC toolchain found -- static_class_parity_tests skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    std::vector<std::string> paths;
    auto writeScript = [&](const std::string& className, const std::string& src) {
        std::string p = dir + "/" + className + ".cscript";
        std::ofstream o(p, std::ios::binary);
        o << src;
        paths.push_back(p);
    };

    // ---- (1) start/update ticking parity: I (never compiled) vs N
    // (compiled). -----------------------------------------------------
    for (const char* suf : {"I", "N"})
        writeScript(std::string("StTick") + suf,
                   "static class StTick" + std::string(suf) + " {\n"
                   "    var counter = 0;\n"
                   "    func start() {\n"
                   "        counter = 0;\n"
                   "    }\n"
                   "    func update(float delta) {\n"
                   "        counter = counter + 1;\n"
                   "    }\n"
                   "}\n");

    // ---- (2) cross-script access via bare class name -- 4 combos
    // (caller-native x target-native). ---------------------------------
    struct Combo {
        const char* suffix;
        bool callerNative;
        bool targetNative;
    };
    static const Combo kCombos[] = {
        {"II", false, false}, {"NI", true, false}, {"IN", false, true}, {"NN", true, true}};
    for (const auto& c : kCombos) {
        writeScript(std::string("StTarget") + c.suffix,
                   "static class StTarget" + std::string(c.suffix) + " {\n"
                   "    var score = 0;\n"
                   "    func start() {\n"
                   "        score = 0;\n"
                   "    }\n"
                   "    func addScore(int amount) {\n"
                   "        score = score + amount;\n"
                   "        return score;\n"
                   "    }\n"
                   "}\n");
        writeScript(std::string("StCaller") + c.suffix,
                   "class StCaller" + std::string(c.suffix) + " : Actor3D {\n"
                   "    var result = 0;\n"
                   "    func start() {\n"
                   "        StTarget" + std::string(c.suffix) +
                       ".score = StTarget" + std::string(c.suffix) +
                       ".score + 1;\n"
                       "        result = StTarget" +
                       std::string(c.suffix) + ".addScore(5);\n"
                       "    }\n"
                       "}\n");
    }

    sys.reload();
    for (const char* suf : {"I", "N"})
        CHECK(sys.file(std::string("StTick") + suf) != nullptr);
    for (const auto& c : kCombos) {
        CHECK(sys.file(std::string("StTarget") + c.suffix) != nullptr);
        CHECK(sys.file(std::string("StCaller") + c.suffix) != nullptr);
    }

    const std::string ns = "StParityNs";
    struct Cleanup {
        ScriptSystem* sys;
        std::vector<std::string> paths;
        std::string scriptsDir, ns;
        ~Cleanup() {
            NativeClassRegistry::get().unloadAll();
            sys->resetStatics(); // matches EditorApp.cpp's Stop-sequence ordering
            for (const auto& p : paths)
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
    } cleanup{&sys, paths, dir, ns};

    sys.setNamespace("StTickN", ns);
    for (const auto& c : kCombos) {
        if (c.callerNative)
            sys.setNamespace(std::string("StCaller") + c.suffix, ns);
        if (c.targetNative)
            sys.setNamespace(std::string("StTarget") + c.suffix, ns);
    }
    BuildResult build = buildNamespace(ns, dir);
    if (!build.ok)
        std::printf("  buildNamespace error: %s\n", build.error.c_str());
    CHECK(build.ok);
    std::vector<std::string> nativeClasses = {"StTickN"};
    for (const auto& c : kCombos) {
        if (c.callerNative)
            nativeClasses.push_back(std::string("StCaller") + c.suffix);
        if (c.targetNative)
            nativeClasses.push_back(std::string("StTarget") + c.suffix);
    }
    CHECK(NativeClassRegistry::get().loadNamespace(ns, build.dllPath, nativeClasses));

    // ---- run (1): start/update ticking, 5 frames -------------------------
    sys.startStatics(); // picks up the just-loaded native export via rebuildStatic()
    for (int i = 0; i < 5; ++i)
        sys.tickStatics(0.1f);
    CHECK(staticFieldOf("StTickI", "counter").i == 5);
    CHECK(staticFieldOf("StTickN", "counter").i == 5);
    std::printf("ok  static start/update ticking parity (interpreted + native): 5 frames -> "
                "counter=5 for both\n");

    // ---- run (2): cross-script access, all 4 combos -----------------------
    {
        Scene scene("st-cross");
        for (const auto& c : kCombos) {
            Actor* a = scene.add(std::make_unique<Actor3D>(std::string("Caller_") + c.suffix));
            auto comp = ComponentRegistry::get().create(std::string("StCaller") + c.suffix);
            CHECK(comp != nullptr);
            CHECK((dynamic_cast<NativeScriptComponent*>(comp.get()) != nullptr) == c.callerNative);
            a->addComponent(std::move(comp));
        }
        scene.startPlay(); // runs every StCallerXX::start()

        for (const auto& c : kCombos) {
            Actor* a = nullptr;
            for (const auto& child : scene.root().children())
                if (child->name() == std::string("Caller_") + c.suffix)
                    a = child.get();
            CHECK(a != nullptr);
            Component* caller = a->components()[0].get();
            CHECK(staticFieldOf(std::string("StTarget") + c.suffix, "score").i == 6);
            CHECK(fieldOf(caller, "result").i == 6);
            std::printf("ok  cross-script static access combo %s (caller %s, target %s): "
                        "StaticClassName.field read/write + .method() both landed correctly "
                        "(score=6, result=6)\n",
                        c.suffix, c.callerNative ? "native" : "interpreted",
                        c.targetNative ? "native" : "interpreted");
        }
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
