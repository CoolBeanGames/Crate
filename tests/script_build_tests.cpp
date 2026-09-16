// Coverage for src/editor/ScriptBuild.h/.cpp (transpiration.txt,
// "Transplation" Phase 3): toolchain discovery, dirty-namespace tracking
// across a fresh scan, and an actual end-to-end namespace build producing a
// real, versioned .dll via cl.exe/link.exe. Skips the compiler-dependent
// checks gracefully (not a failure) on a machine with no MSVC toolchain
// discoverable via vswhere.exe -- ToolchainEnv::available() itself is
// always checked, never assumed true.
//
// Uses the real assets/scripts/ directory (via CRATE_SCRIPTS_DIR, same
// convention as script_integration_tests.cpp / script_metadata_tests.cpp):
// temp .cscript files are written in, assigned to a throwaway namespace,
// exercised, then removed via reload() afterward, and their namespace
// overrides + generated .crate_build state are cleaned up so nothing is
// left behind in the repo (matching script_metadata_tests.cpp's cleanup
// discipline).
//
// No framework: asserts + a pass counter, run via CTest.

#include "editor/ScriptBuild.h"
#include "script/ScriptSystem.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace crate;
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

static void writeScript(const std::string& path, const std::string& src) {
    std::ofstream o(path, std::ios::binary);
    o << src;
}

int main() {
    auto& sys = script::ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    // --- ToolchainEnv --------------------------------------------------
    auto& tc = ToolchainEnv::get();
    bool haveToolchain = tc.available();
    if (!haveToolchain)
        std::printf(
            "(no MSVC toolchain found via vswhere.exe -- compiler-dependent checks skipped: %s)\n",
            tc.error().c_str());
    else
        std::printf("ok  ToolchainEnv::available() -- MSVC toolchain discovered\n");

    if (haveToolchain) {
        std::string out;
        int rc = tc.run("cl.exe /nologo /?", dir, out);
        CHECK(rc != -1); // process actually launched
        std::printf("ok  ToolchainEnv::run() launches a real process (cl.exe /?, exit %d)\n", rc);
    }

    // --- namespace setup: two scripts sharing a throwaway namespace -----
    const std::string ns = "SBTestNamespace";
    const std::string pA = dir + "/SBTestAlpha.cscript";
    const std::string pB = dir + "/SBTestBeta.cscript";
    writeScript(pA, "class SBTestAlpha : Actor { var n = 1; func update(float delta) { n = n + 1; } }");
    writeScript(pB, "class SBTestBeta : Actor { func update(float delta) {} }");
    sys.reload();
    CHECK(sys.file("SBTestAlpha") != nullptr);
    CHECK(sys.file("SBTestBeta") != nullptr);
    sys.setNamespace("SBTestAlpha", ns);
    sys.setNamespace("SBTestBeta", ns);

    // A cleanup guard so every exit path (including early CHECK failures)
    // still removes the temp files and namespace overrides. Not exception-
    // based (this codebase's tests just `return 1` on failure) -- called
    // explicitly at the bottom AND mirrored here via a scope guard for the
    // early-return CHECK macro paths above/below.
    struct Cleanup {
        script::ScriptSystem* sys;
        std::string a, b, scriptsDir, ns;
        ~Cleanup() {
            std::remove(a.c_str());
            std::remove(b.c_str());
            sys->reload(); // also drops the namespace override (see script_metadata_tests.cpp)
            // Best-effort: remove this test's generated build state so
            // repeated runs don't accumulate v3, v4, ... forever. Not part
            // of ScriptBuild's own public API (no cleanup function exists
            // yet -- old-generation sweeping is a documented deferred item,
            // see transpiration.txt Phase 3) so done here directly.
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
            // Also drop this namespace's manifest.tsv entry, so a repeated
            // test run starts generation numbering fresh (the test itself
            // never asserts a specific version number, only that it
            // increases -- see below -- but a persistent, unbounded
            // manifest entry from a throwaway test namespace is pure
            // clutter, unlike a real namespace's manifest entry, which is
            // supposed to persist).
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
    } cleanup{&sys, pA, pB, dir, ns};

    // --- dirty tracking: a never-built namespace is dirty ---------------
    {
        auto dirty = computeDirtyNamespaces(dir);
        CHECK(dirty.count(ns) == 1);
        std::printf("ok  a never-built namespace is reported dirty\n");
    }

    // --- computeRebuildSet: identity function today (see ScriptBuild.h) -
    {
        std::unordered_set<std::string> in = {ns, "SomeOtherNamespace"};
        auto out = computeRebuildSet(in, dir);
        CHECK(out == in);
        std::printf("ok  computeRebuildSet is the identity function (no inheritance support yet)\n");
    }

    if (!haveToolchain) {
        std::printf("ok  %d checks passed (compiler-dependent build checks skipped)\n", g_checks);
        return 0;
    }

    // --- actual build: produces a real, versioned DLL --------------------
    // NOTE: the manifest persists ACROSS test runs by design (it's a real
    // build cache, not per-run scratch state), so this must not assume the
    // first build in a given run lands on generation 1 -- only that it
    // produces a plausibly-named, real file, and that a rebuild strictly
    // increases the generation from whatever it started at.
    BuildResult r1 = buildNamespace(ns, dir);
    if (!r1.ok)
        std::printf("  buildNamespace error: %s\n", r1.error.c_str());
    CHECK(r1.ok);
    std::error_code ec;
    CHECK(fs::exists(r1.dllPath, ec));
    CHECK(r1.dllPath.find(ns + "_v") != std::string::npos);
    CHECK(r1.dllPath.rfind(".dll") == r1.dllPath.size() - 4);
    std::printf("ok  buildNamespace() produces %s\n", fs::path(r1.dllPath).filename().string().c_str());

    // --- after a successful build, the namespace is no longer dirty -----
    {
        auto dirty = computeDirtyNamespaces(dir);
        CHECK(dirty.count(ns) == 0);
        std::printf("ok  a freshly-built, unchanged namespace is no longer dirty\n");
    }

    // --- changing a script's source makes the namespace dirty again, and
    // rebuilding bumps the generation (a NEW, differently-named DLL, not an
    // overwrite -- Windows won't let a loaded DLL's file be overwritten
    // anyway; this is the shadow-copy scheme transpiration.txt calls for).
    sys.setSource("SBTestAlpha",
                  "class SBTestAlpha : Actor { var n = 100; func update(float delta) { n = n + 1; } }");
    {
        auto dirty = computeDirtyNamespaces(dir);
        CHECK(dirty.count(ns) == 1);
        std::printf("ok  editing one script in a namespace makes it dirty again\n");
    }

    BuildResult r2 = buildNamespace(ns, dir);
    if (!r2.ok)
        std::printf("  buildNamespace error: %s\n", r2.error.c_str());
    CHECK(r2.ok);
    CHECK(r2.dllPath.find(ns + "_v") != std::string::npos);
    CHECK(r2.dllPath != r1.dllPath);
    CHECK(fs::exists(r1.dllPath, ec)); // v1 is left in place, not deleted/overwritten
    CHECK(fs::exists(r2.dllPath, ec));
    // The generation strictly increases: parse "<ns>_v<N>.dll" out of each.
    auto genOf = [&](const std::string& p) {
        std::string stem = fs::path(p).stem().string(); // "<ns>_v<N>"
        std::string prefix = ns + "_v";
        return std::stoi(stem.substr(prefix.size()));
    };
    CHECK(genOf(r2.dllPath) == genOf(r1.dllPath) + 1);
    std::printf("ok  a rebuild bumps the generation to a new, distinctly-named DLL (v1 untouched)\n");

    // --- building a namespace with no scripts fails cleanly --------------
    {
        BuildResult r = buildNamespace("SBTestNamespaceThatDoesNotExist", dir);
        CHECK(!r.ok);
        CHECK(!r.error.empty());
        std::printf("ok  building an empty/unknown namespace fails cleanly: %s\n", r.error.c_str());
    }

    // --- REGRESSION: buildNamespace() must work with a RELATIVE scriptsDir
    // argument too, not just an absolute one. This is exactly the bug
    // manual testing in the real editor caught: EditorApp passes
    // ScriptSystem::scriptsDir(), which in the real app is the relative
    // path "assets/scripts" (assetDir_ defaults to "assets"), whereas every
    // OTHER test in this file uses CRATE_SCRIPTS_DIR, an ABSOLUTE compile-
    // time path -- so this specific failure mode was invisible everywhere
    // else. buildNamespace() must internally resolve to an absolute path
    // before using it as both a subprocess cwd and a source of command-line
    // argument text (see ScriptBuild.cpp's own comment on this). -----------
    {
        std::error_code relEc;
        std::string relDir = fs::relative(dir, fs::current_path(), relEc).generic_string();
        // Only meaningful if a relative path actually exists between CWD
        // and the scripts dir (always true here: both are under the same
        // repo checkout) -- skip defensively rather than fail outright if
        // some exotic environment ever put them on different drives.
        if (!relEc && !relDir.empty() && relDir != ".") {
            sys.setSource("SBTestAlpha",
                          "class SBTestAlpha : Actor { var n = 7; func update(float delta) {} }");
            BuildResult rRel = buildNamespace(ns, relDir);
            if (!rRel.ok)
                std::printf("  buildNamespace (relative path '%s') error: %s\n", relDir.c_str(),
                           rRel.error.c_str());
            CHECK(rRel.ok);
            CHECK(fs::exists(rRel.dllPath, ec));
            std::printf("ok  buildNamespace() works with a RELATIVE scriptsDir argument (regression "
                       "test)\n");
        } else {
            std::printf(
                "(skipping relative-scriptsDir regression check -- no relative path available)\n");
        }
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
