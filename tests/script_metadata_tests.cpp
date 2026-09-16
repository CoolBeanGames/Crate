// Coverage for ScriptSystem's per-script "namespace" metadata (transpiration.txt,
// Phase 1): default value, persistence to .scriptmeta across a fresh load,
// following a class rename, and cleanup on delete. No framework: asserts + a
// pass counter, run via CTest (matches every other test file in this repo).
//
// Uses the real shipped assets/scripts/ directory (via CRATE_SCRIPTS_DIR),
// same convention as script_integration_tests.cpp: temp .cscript files are
// written in, exercised, then removed via reload() so nothing is left behind
// in the repo. Every namespace override this file creates is explicitly
// reset back to "Global" (which erases it) before exiting, so an existing
// developer's real .scriptmeta entries for unrelated scripts are untouched.

#include "script/ScriptSystem.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace crate;

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

static std::string readWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    auto& sys = script::ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    // --- default namespace -------------------------------------------------
    {
        // A class name never seen before: default is "Global", by reference
        // (no throw / no crash) even though nothing was ever set for it.
        CHECK(sys.namespaceOf("NsTestNeverSeen") == "Global");
        CHECK(sys.namespaceOf("") == "Global");
        std::printf("ok  unset script defaults to namespace \"Global\"\n");
    }

    // --- set / get round-trip, in-memory ------------------------------------
    const std::string pAlpha = dir + "/NsTestAlpha.cscript";
    {
        std::ofstream o(pAlpha, std::ios::binary);
        o << "class NsTestAlpha : Actor { func update(float d) {} }";
    }
    sys.reload();
    CHECK(sys.file("NsTestAlpha") != nullptr);
    CHECK(sys.namespaceOf("NsTestAlpha") == "Global"); // still default right after creation

    sys.setNamespace("NsTestAlpha", "Gameplay");
    CHECK(sys.namespaceOf("NsTestAlpha") == "Gameplay");
    std::printf("ok  setNamespace/namespaceOf round-trip in memory\n");

    // --- persists to disk across a fresh load -------------------------------
    {
        // Simulate the process restarting: reload the same folder from
        // scratch (loadFolder always re-reads .scriptmeta, see
        // ScriptSystem::loadFolder -> loadNamespaces()).
        sys.loadFolder(dir);
        CHECK(sys.namespaceOf("NsTestAlpha") == "Gameplay");
        std::printf("ok  namespace override survives a fresh loadFolder() (disk persistence)\n");
    }

    // --- the .scriptmeta file itself is tab-separated, one line per override
    {
        std::string contents = readWholeFile(dir + "/.scriptmeta");
        CHECK(contents.find("NsTestAlpha\tGameplay") != std::string::npos);
        std::printf("ok  .scriptmeta on-disk format is \"className<TAB>namespace\"\n");
    }

    // --- a second, independent script defaults separately -------------------
    const std::string pBeta = dir + "/NsTestBeta.cscript";
    {
        std::ofstream o(pBeta, std::ios::binary);
        o << "class NsTestBeta : Actor { func update(float d) {} }";
    }
    sys.reload();
    CHECK(sys.namespaceOf("NsTestBeta") == "Global"); // unaffected by Alpha's override
    CHECK(sys.namespaceOf("NsTestAlpha") == "Gameplay"); // Alpha's override still intact
    sys.setNamespace("NsTestBeta", "Physics");
    CHECK(sys.namespaceOf("NsTestBeta") == "Physics");
    CHECK(sys.namespaceOf("NsTestAlpha") == "Gameplay"); // setting Beta didn't disturb Alpha
    std::printf("ok  independent scripts keep independent namespace overrides\n");

    // --- setting back to "Global" (or "") erases the override, not just
    // shadows it -- verified directly against the file, not just namespaceOf,
    // since namespaceOf("Global") is indistinguishable from "never set" from
    // the getter alone.
    sys.setNamespace("NsTestBeta", "Global");
    CHECK(sys.namespaceOf("NsTestBeta") == "Global");
    {
        std::string contents = readWholeFile(dir + "/.scriptmeta");
        CHECK(contents.find("NsTestBeta\t") == std::string::npos); // no line for Beta anymore
        CHECK(contents.find("NsTestAlpha\tGameplay") != std::string::npos); // Alpha untouched
    }
    sys.setNamespace("NsTestAlpha", ""); // empty string also means "clear"
    CHECK(sys.namespaceOf("NsTestAlpha") == "Global");
    {
        std::string contents = readWholeFile(dir + "/.scriptmeta");
        CHECK(contents.find("NsTestAlpha\t") == std::string::npos);
    }
    std::printf("ok  setNamespace(\"Global\"/\"\") erases the override rather than storing it\n");

    // --- follows a class rename ---------------------------------------------
    sys.setNamespace("NsTestAlpha", "Rendering");
    CHECK(sys.namespaceOf("NsTestAlpha") == "Rendering");
    std::string renamed = sys.setSource(
        "NsTestAlpha", "class NsTestAlphaRenamed : Actor { func update(float d) {} }");
    CHECK(renamed == "NsTestAlphaRenamed");
    CHECK(sys.namespaceOf("NsTestAlphaRenamed") == "Rendering"); // override followed the rename
    CHECK(sys.namespaceOf("NsTestAlpha") == "Global"); // old name has nothing (default again)
    {
        std::string contents = readWholeFile(dir + "/.scriptmeta");
        CHECK(contents.find("NsTestAlphaRenamed\tRendering") != std::string::npos);
        CHECK(contents.find("NsTestAlpha\tRendering") == std::string::npos); // old key gone
    }
    std::printf("ok  namespace override follows a class rename\n");

    // --- cleanup on delete ---------------------------------------------------
    sys.setNamespace("NsTestAlphaRenamed", "Rendering"); // (already set, re-affirm before delete)
    std::remove(pAlpha.c_str()); // note: file on disk is still named NsTestAlpha.cscript
    std::remove(pBeta.c_str());
    sys.reload();
    CHECK(sys.file("NsTestAlphaRenamed") == nullptr);
    CHECK(sys.file("NsTestBeta") == nullptr);
    // The override for the deleted (renamed) class must be gone too, not
    // orphaned in .scriptmeta forever.
    CHECK(sys.namespaceOf("NsTestAlphaRenamed") == "Global");
    {
        std::string contents = readWholeFile(dir + "/.scriptmeta");
        CHECK(contents.find("NsTestAlphaRenamed") == std::string::npos);
        CHECK(contents.find("NsTestBeta") == std::string::npos);
        CHECK(contents.find("NsTestAlpha") == std::string::npos);
    }
    std::printf("ok  namespace override is dropped when its script is deleted (reload cleanup)\n");

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
