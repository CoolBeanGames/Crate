// Phase 9f end-to-end parity test: SAME-NAMESPACE script-to-script
// inheritance compiled to real C++ inheritance -- field override, inherited
// method calls, inherited field read/write, this.base.method(), and an
// inherited signal, all compared interpreted vs compiled. This is the
// "simplest" stepping-stone test the plan calls for before attempting
// cross-namespace inheritance (not yet implemented -- see
// transpiration.txt Phase 9f's own notes on what's left). See
// transpiration.txt, "Transplation" Phase 9f.
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

static Value fieldOf(Component* c, const std::string& name) {
    if (auto* sc = dynamic_cast<ScriptComponent*>(c))
        return sc->object()->fields.at(name);
    if (auto* nc = dynamic_cast<NativeScriptComponent*>(c)) {
        auto view = nc->ensureNativeView();
        for (size_t i = 0; i < view.classInfo->fieldCount; ++i)
            if (view.classInfo->fields[i].name == name)
                return view.classInfo->fields[i].get(view.instance);
        // Phase 9f: an inherited (not overridden) field isn't in the
        // DERIVED class's own field table -- walk to its base's, exactly
        // like getObjectMember/ObjectDispatch.cpp's findCompiledField does
        // at runtime for script code.
        for (const CompiledClassInfo* ci = view.classInfo->baseClassInfo; ci; ci = ci->baseClassInfo)
            for (size_t i = 0; i < ci->fieldCount; ++i)
                if (ci->fields[i].name == name)
                    return ci->fields[i].get(view.instance);
        std::fprintf(stderr, "fieldOf: native field '%s' not found (own or inherited)\n",
                    name.c_str());
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
        std::printf("(no MSVC toolchain found -- inheritance_parity_tests skipped: %s)\n",
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

    for (const char* suf : {"I", "N"}) {
        writeScript(std::string("InhBase") + suf,
                   "class InhBase" + std::string(suf) + " : Actor3D {\n"
                   "    signal died(cause);\n"
                   "    var health = 100;\n"
                   "    var untouched = 1;\n"
                   "    func heal(int amount) {\n"
                   "        health = health + amount;\n"
                   "        return health;\n"
                   "    }\n"
                   "    func describe() {\n"
                   "        return \"base\";\n"
                   "    }\n"
                   "}\n");
        writeScript(std::string("InhDerived") + suf,
                   "class InhDerived" + std::string(suf) + " : InhBase" + std::string(suf) +
                       " {\n"
                       "    var health = 50;\n"
                       "    var lastCause = \"\";\n"
                       "    var lastDescribe = \"\";\n"
                       "    func onDied(cause) {\n"
                       "        lastCause = cause;\n"
                       "    }\n"
                       "    func describe() {\n"
                       "        var baseDesc = this.base.describe();\n"
                       "        return baseDesc + \"+derived\";\n"
                       "    }\n"
                       "    func start() {\n"
                       "        this.died.connect(onDied);\n"
                       "        var h1 = heal(10);\n" // inherited method
                       "        untouched = untouched + 5;\n" // inherited field write
                       "        lastDescribe = describe();\n" // overridden method + this.base
                       "        this.emit_signal(\"died\", \"fell\");\n" // inherited signal
                       "    }\n"
                       "}\n");
    }

    sys.reload();
    for (const char* suf : {"I", "N"}) {
        CHECK(sys.file(std::string("InhBase") + suf) != nullptr);
        CHECK(sys.file(std::string("InhDerived") + suf) != nullptr);
    }

    const std::string ns = "InhParityNs";
    struct Cleanup {
        ScriptSystem* sys;
        std::vector<std::string> paths;
        std::string scriptsDir, ns;
        ~Cleanup() {
            NativeClassRegistry::get().unloadAll();
            sys->resetStatics();
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

    // Both "N" classes go into the SAME namespace, compiled together --
    // same-namespace inheritance is the ONLY kind wired up so far.
    sys.setNamespace("InhBaseN", ns);
    sys.setNamespace("InhDerivedN", ns);
    BuildResult build = buildNamespace(ns, dir);
    if (!build.ok)
        std::printf("  buildNamespace error: %s\n", build.error.c_str());
    CHECK(build.ok);
    CHECK(NativeClassRegistry::get().loadNamespace(ns, build.dllPath, {"InhBaseN", "InhDerivedN"}));

    // ---- run both (I never compiled, N compiled together) ----------------
    Scene scene("inh-parity");
    for (const char* suf : {"I", "N"}) {
        Actor* a = scene.add(std::make_unique<Actor3D>(std::string("Derived_") + suf));
        auto comp = ComponentRegistry::get().create(std::string("InhDerived") + suf);
        CHECK(comp != nullptr);
        bool isNative = dynamic_cast<NativeScriptComponent*>(comp.get()) != nullptr;
        CHECK(isNative == (std::string(suf) == "N"));
        a->addComponent(std::move(comp));
    }
    scene.startPlay();

    for (const char* suf : {"I", "N"}) {
        Actor* a = nullptr;
        for (const auto& child : scene.root().children())
            if (child->name() == std::string("Derived_") + suf)
                a = child.get();
        CHECK(a != nullptr);
        Component* c = a->components()[0].get();

        CHECK(fieldOf(c, "health").i == 60);    // overridden default (50) + inherited heal(10)
        CHECK(fieldOf(c, "untouched").i == 6);  // inherited field (1), written by derived (+5)
        CHECK(fieldOf(c, "lastDescribe").s == "base+derived"); // this.base.describe() + own
        CHECK(fieldOf(c, "lastCause").s == "fell"); // inherited signal, connected + emitted
        std::printf("ok  same-namespace inheritance parity (%s): field override (health=60), "
                    "inherited field write (untouched=6), this.base.method() "
                    "(lastDescribe='base+derived'), inherited signal (lastCause='fell')\n",
                    std::string(suf) == "N" ? "native" : "interpreted");
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
