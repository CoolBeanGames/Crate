// Checks for the shared project-creation/lookup helpers added for the
// launcher + `crate` CLI (task 125): createProjectInFolder() and
// findCrateFileInFolder(). No framework: asserts + a pass counter, run via
// CTest, matching project_file_tests.cpp's style.

#include "editor/ProjectFile.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace crate;
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
    fs::path dir = fs::temp_directory_path() / "crate_project_ops_tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // Happy path: creates <dir>/Proj/{Proj.crate, assets/Default.inputmap}.
    {
        fs::path projectDir = dir / "Proj";
        CreateProjectResult r = createProjectInFolder(projectDir.generic_string(), "Proj");
        CHECK(r.ok);
        CHECK(r.error.empty());
        CHECK(fs::exists(projectDir / "Proj.crate"));
        CHECK(fs::exists(projectDir / "assets" / "Default.inputmap"));
        CHECK(r.crateFilePath == (projectDir / "Proj.crate").generic_string());
        CHECK(r.assetsDir == (projectDir / "assets").generic_string());

        ProjectInfo info;
        CHECK(loadProjectFile(r.crateFilePath, &info));
        CHECK(info.name == "Proj");

        // findCrateFileInFolder finds exactly the one we just made.
        std::string error;
        std::string found = findCrateFileInFolder(projectDir.generic_string(), &error);
        CHECK(found == r.crateFilePath);
        CHECK(error.empty());
    }

    // An existing, EMPTY folder is fine to build into (e.g. one the user
    // pre-made out of habit).
    {
        fs::path projectDir = dir / "PreMade";
        fs::create_directories(projectDir, ec);
        CreateProjectResult r = createProjectInFolder(projectDir.generic_string(), "PreMade");
        CHECK(r.ok);
    }

    // An existing, NON-empty folder is refused rather than silently merged
    // into / overwritten.
    {
        fs::path projectDir = dir / "Occupied";
        fs::create_directories(projectDir, ec);
        std::ofstream(projectDir / "something.txt") << "not ours";
        CreateProjectResult r = createProjectInFolder(projectDir.generic_string(), "Occupied");
        CHECK(!r.ok);
        CHECK(!r.error.empty());
        // The pre-existing file must survive untouched.
        CHECK(fs::exists(projectDir / "something.txt"));
        CHECK(!fs::exists(projectDir / "Occupied.crate"));
    }

    // findCrateFileInFolder: zero .crate files is an error, not empty-success.
    {
        fs::path emptyDir = dir / "NoCrateHere";
        fs::create_directories(emptyDir, ec);
        std::string error;
        CHECK(findCrateFileInFolder(emptyDir.generic_string(), &error).empty());
        CHECK(!error.empty());
    }

    // findCrateFileInFolder: more than one .crate file is ambiguous, refused.
    {
        fs::path ambiguousDir = dir / "TwoCrates";
        fs::create_directories(ambiguousDir, ec);
        std::ofstream(ambiguousDir / "A.crate") << "CRATE_PROJECT 1\nNAME A\n";
        std::ofstream(ambiguousDir / "B.crate") << "CRATE_PROJECT 1\nNAME B\n";
        std::string error;
        CHECK(findCrateFileInFolder(ambiguousDir.generic_string(), &error).empty());
        CHECK(!error.empty());
    }

    // findCrateFileInFolder on a folder that doesn't exist fails cleanly.
    {
        std::string error;
        CHECK(findCrateFileInFolder((dir / "DoesNotExist").generic_string(), &error).empty());
        CHECK(!error.empty());
    }

    fs::remove_all(dir, ec);
    std::printf("OK project_ops_tests (%d checks)\n", g_checks);
    return 0;
}
