// Phase 9f (cross-namespace part) end-to-end test: a base class and a
// derived class living in TWO SEPARATE namespaces (two separate DLLs),
// covering the full pipeline computeDirtyNamespaces() didn't need to touch
// but computeRebuildSet()'s new dependency-graph/topological-sort logic
// does -- real cross-DLL __declspec(dllexport)/dllimport class export,
// -I<base's gen dir> and the base's CURRENT .lib on the derived namespace's
// build commands, field override, an inherited method call, this.base.
// method(), and an inherited signal, all through TWO real, separately
// built and linked DLLs. Also confirms the "known existing gap" scenario
// this sub-phase's own plan text called out closes for real: changing only
// the BASE namespace's source correctly rebuilds the DERIVED namespace too
// (computeRebuildSet's dependency expansion), rather than silently leaving
// it linked against a stale layout. See transpiration.txt, "Transplation"
// Phase 9f.
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

#include <algorithm>
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
        std::printf("(no MSVC toolchain found -- cross_namespace_inheritance_tests skipped: %s)\n",
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

    const std::string baseSrcV1 = "class XnsBase : Actor3D {\n"
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
                                  "}\n";
    writeScript("XnsBase", baseSrcV1);
    writeScript("XnsDerived", "class XnsDerived : XnsBase {\n"
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
                              "        var h1 = heal(10);\n"
                              "        untouched = untouched + 5;\n"
                              "        lastDescribe = describe();\n"
                              "        this.emit_signal(\"died\", \"fell\");\n"
                              "    }\n"
                              "}\n");

    sys.reload();
    CHECK(sys.file("XnsBase") != nullptr);
    CHECK(sys.file("XnsDerived") != nullptr);

    const std::string baseNs = "XnsBaseNs";
    const std::string derivedNs = "XnsDerivedNs";
    struct Cleanup {
        ScriptSystem* sys;
        std::vector<std::string> paths;
        std::string scriptsDir;
        std::vector<std::string> namespaces;
        ~Cleanup() {
            NativeClassRegistry::get().unloadAll();
            sys->resetStatics();
            for (const auto& p : paths)
                std::remove(p.c_str());
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
    } cleanup{&sys, paths, dir, {baseNs, derivedNs}};

    sys.setNamespace("XnsBase", baseNs);
    sys.setNamespace("XnsDerived", derivedNs);

    // ---- computeRebuildSet: dependency expansion + topological order ----
    {
        std::unordered_set<std::string> dirty = {baseNs, derivedNs};
        auto order = computeRebuildSet(dirty, dir);
        auto baseIt = std::find(order.begin(), order.end(), baseNs);
        auto derivedIt = std::find(order.begin(), order.end(), derivedNs);
        CHECK(baseIt != order.end());
        CHECK(derivedIt != order.end());
        CHECK(baseIt < derivedIt); // base namespace ordered BEFORE its dependent
        std::printf("ok  computeRebuildSet topologically orders %s before %s\n", baseNs.c_str(),
                    derivedNs.c_str());
    }
    {
        // The "known existing gap" scenario (Phase 3's own note): only the
        // BASE namespace is dirty -- the derived one must still be
        // included (and still ordered after), since its compiled layout
        // depends on the base's.
        std::unordered_set<std::string> dirty = {baseNs};
        auto order = computeRebuildSet(dirty, dir);
        auto baseIt = std::find(order.begin(), order.end(), baseNs);
        auto derivedIt = std::find(order.begin(), order.end(), derivedNs);
        CHECK(baseIt != order.end());
        CHECK(derivedIt != order.end()); // dependency expansion pulled it in
        CHECK(baseIt < derivedIt);
        std::printf("ok  computeRebuildSet expands a dirty BASE namespace to include its "
                    "DEPENDENT namespace too (the 'known existing gap' scenario)\n");
    }

    // ---- real build: base namespace first, then the derived one --------
    BuildResult baseBuild = buildNamespace(baseNs, dir);
    if (!baseBuild.ok)
        std::printf("  buildNamespace(%s) error: %s\n", baseNs.c_str(), baseBuild.error.c_str());
    CHECK(baseBuild.ok);
    CHECK(NativeClassRegistry::get().loadNamespace(baseNs, baseBuild.dllPath, {"XnsBase"}));

    BuildResult derivedBuild = buildNamespace(derivedNs, dir);
    if (!derivedBuild.ok)
        std::printf("  buildNamespace(%s) error: %s\n", derivedNs.c_str(), derivedBuild.error.c_str());
    CHECK(derivedBuild.ok);
    CHECK(NativeClassRegistry::get().loadNamespace(derivedNs, derivedBuild.dllPath, {"XnsDerived"}));

    // ---- run: derived instance, base compiled into a DIFFERENT DLL ------
    Scene scene("xns-parity");
    Actor* a = scene.add(std::make_unique<Actor3D>("Derived"));
    auto comp = ComponentRegistry::get().create("XnsDerived");
    CHECK(comp != nullptr);
    CHECK(dynamic_cast<NativeScriptComponent*>(comp.get()) != nullptr); // really native
    Component* derived = a->addComponent(std::move(comp));
    scene.startPlay();

    CHECK(fieldOf(derived, "health").i == 60);   // override (50) + inherited heal(10)
    CHECK(fieldOf(derived, "untouched").i == 6); // inherited field (1), +5
    CHECK(fieldOf(derived, "lastDescribe").s == "base+derived"); // this.base.describe()
    CHECK(fieldOf(derived, "lastCause").s == "fell"); // inherited signal
    std::printf("ok  cross-namespace inheritance (base in %s, derived in %s, two SEPARATE DLLs): "
                "field override (health=60), inherited field write (untouched=6), "
                "this.base.method() (lastDescribe='base+derived'), inherited signal "
                "(lastCause='fell')\n",
                baseNs.c_str(), derivedNs.c_str());

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
