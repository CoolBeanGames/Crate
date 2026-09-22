// Tasks 144/145: "New Script" and externally-added-script recognition.
// No framework: asserts + a pass counter, run via CTest, matching
// project_ops_tests.cpp's style. Exercises ScriptSystem directly -- no UI --
// so it stays meaningful without any interactive automation.

#include "script/ScriptSystem.h"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace crate::script;
namespace fs = std::filesystem;

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
    // Sanity check the exact comparison EditorApp.cpp's Asset Browser uses to
    // match a tile's forward-slash generic_string() path against
    // ScriptSystem's own native-separator ScriptFile::path -- if THIS were
    // ever false on Windows, no script (old or new) could ever be opened by
    // double-clicking its tile, so it's foundational to both bugs below.
    CHECK(fs::path("C:\\proj\\assets\\scripts\\Foo.cscript") ==
          fs::path("C:/proj/assets/scripts/Foo.cscript"));

    fs::path dir = fs::temp_directory_path() / "crate_scripting_new_reload_tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    auto& sys = ScriptSystem::get();

    // --- Task 144: New Script ------------------------------------------------
    {
        sys.unloadAll();
        sys.loadFolder(dir.generic_string()); // empty project, like a fresh mount
        std::string name = sys.newScript("PlayerControl");
        CHECK(name == "PlayerControl");

        fs::path expected = dir / "scripts" / "PlayerControl.cscript";
        CHECK(fs::exists(expected, ec));

        auto* f = sys.file("PlayerControl");
        CHECK(f != nullptr);
        CHECK(f->error.empty());
        CHECK(!f->path.empty());
        // The exact comparison the Asset Browser tile loop performs, using a
        // generic_string() built the same way scanning the folder would.
        CHECK(fs::path(f->path) == fs::path(expected.generic_string()));

        // A second New Script with the same requested name must not collide.
        std::string name2 = sys.newScript("PlayerControl");
        CHECK(name2 == "PlayerControl2");
        CHECK(fs::exists(dir / "scripts" / "PlayerControl2.cscript", ec));
    }

    // --- Task 145: externally-added file recognized after reload() ----------
    {
        fs::remove_all(dir, ec); // clean slate -- section above left 2 files
        fs::create_directories(dir, ec);
        sys.unloadAll();
        sys.loadFolder(dir.generic_string());
        CHECK(sys.files().size() == 0);

        // Simulate dragging a .cscript file in via Windows Explorer: write it
        // directly to disk with no ScriptSystem API involved, exactly like an
        // external drop, INCLUDING into a subfolder scripts/ doesn't already
        // know about (task 132: scripts can live anywhere under the project).
        fs::path externalDir = dir / "imported_from_another_project";
        fs::create_directories(externalDir, ec);
        fs::path externalFile = externalDir / "Grenade.cscript";
        {
            std::ofstream out(externalFile, std::ios::binary);
            out << "class Grenade : Actor\n{\n\tfunc start()\n\t{\n\t}\n}\n";
        }

        sys.reload();

        auto* f = sys.file("Grenade");
        CHECK(f != nullptr);
        CHECK(f->error.empty());
        CHECK(fs::path(f->path) == fs::path(externalFile.generic_string()));

        // And a second reload() with nothing new changed must not duplicate it.
        sys.reload();
        int count = 0;
        for (auto& sf : sys.files())
            if (sf.name == "Grenade")
                ++count;
        CHECK(count == 1);
    }

    // --- reload() dedup survives a case-only path difference -----------------
    // (found live: newScript() hardcodes a lowercase "scripts" folder name,
    // but if that folder already exists on disk with different case -- e.g.
    // a pre-existing "Scripts" -- Windows resolves to the same physical
    // folder while the ScriptFile::path string recorded at creation time
    // keeps whatever case was used to construct it, differing from what a
    // later directory_iterator reports back for that identical file).
    {
        auto* f = sys.file("Foo_CaseTest");
        CHECK(f == nullptr); // not created yet
        fs::path caseFile = dir / "scripts" / "Foo_CaseTest.cscript";
        fs::create_directories(caseFile.parent_path(), ec);
        {
            std::ofstream out(caseFile, std::ios::binary);
            out << "class Foo_CaseTest : Actor\n{\n\tfunc start()\n\t{\n\t}\n}\n";
        }
        sys.reload();
        f = sys.file("Foo_CaseTest");
        CHECK(f != nullptr);
        // Simulate the path having been recorded with different case than
        // what's physically on disk (exactly what newScript()'s hardcoded
        // lowercase folder name produces against a pre-existing
        // differently-cased folder).
        std::string upper = f->path;
        for (char& c : upper)
            c = (char)std::toupper((unsigned char)c);
        f->path = upper;
        sys.reload(); // must recognize this as the SAME file, not duplicate it
        int count = 0;
        for (auto& sf : sys.files())
            if (sf.name == "Foo_CaseTest")
                ++count;
        CHECK(count == 1);
    }

    // --- Externally-added file whose class name mismatches its filename -----
    // (a script renamed via the Script Editor without renaming the file, or
    // one imported from elsewhere under a different filename convention).
    {
        fs::path oddFile = dir / "scripts" / "OldFileName.cscript";
        fs::create_directories(oddFile.parent_path(), ec);
        {
            std::ofstream out(oddFile, std::ios::binary);
            out << "class RenamedClass : Actor\n{\n\tfunc start()\n\t{\n\t}\n}\n";
        }
        sys.reload();
        auto* f = sys.file("RenamedClass");
        CHECK(f != nullptr);
        CHECK(f->error.empty());
        CHECK(fs::path(f->path) == fs::path(oddFile.generic_string()));
    }

    // --- Externally-added file that fails to compile is still listed --------
    // (with an error, not silently dropped) so the user can see and fix it.
    {
        fs::path badFile = dir / "scripts" / "Broken.cscript";
        {
            std::ofstream out(badFile, std::ios::binary);
            out << "this is not valid cscript {{{ \n";
        }
        sys.reload();
        auto* f = sys.file("Broken");
        CHECK(f != nullptr);
        CHECK(!f->error.empty());
    }

    sys.unloadAll();
    fs::remove_all(dir, ec);
    std::printf("OK scripting_new_and_reload_tests (%d checks)\n", g_checks);
    return 0;
}
