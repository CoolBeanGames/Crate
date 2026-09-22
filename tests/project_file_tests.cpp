// Checks for the .crate project file format (task 92). No framework: asserts
// + a pass counter, run via CTest.

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
    fs::path dir = fs::temp_directory_path() / "crate_project_file_tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // Round-trip: save then load gives back the same info.
    {
        fs::path p = dir / "RoundTrip.crate";
        ProjectInfo info;
        info.name = "RoundTrip";
        std::string error;
        CHECK(saveProjectFile(p.string(), info, &error));
        CHECK(error.empty());
        CHECK(fs::exists(p));

        ProjectInfo loaded;
        CHECK(loadProjectFile(p.string(), &loaded, &error));
        CHECK(loaded.name == "RoundTrip");
    }

    // A name containing spaces survives the round trip (rest-of-line value).
    {
        fs::path p = dir / "SpacedName.crate";
        ProjectInfo info;
        info.name = "My Cool Game";
        CHECK(saveProjectFile(p.string(), info));
        ProjectInfo loaded;
        CHECK(loadProjectFile(p.string(), &loaded));
        CHECK(loaded.name == "My Cool Game");
    }

    // Loading a nonexistent file fails cleanly with an error message.
    {
        ProjectInfo loaded;
        std::string error;
        CHECK(!loadProjectFile((dir / "DoesNotExist.crate").string(), &loaded, &error));
        CHECK(!error.empty());
    }

    // Loading a file that isn't a project file (wrong header) fails.
    {
        fs::path p = dir / "NotAProject.txt";
        std::ofstream(p.string()) << "just some random text\nNAME whatever\n";
        ProjectInfo loaded;
        std::string error;
        CHECK(!loadProjectFile(p.string(), &loaded, &error));
        CHECK(!error.empty());
    }

    // Saving to a directory that doesn't exist fails cleanly rather than
    // throwing or crashing.
    {
        std::string error;
        CHECK(!saveProjectFile((dir / "missing_subdir" / "X.crate").string(), ProjectInfo{}, &error));
        CHECK(!error.empty());
    }

    // outInfo/errorOut are both optional (nullptr-safe).
    {
        fs::path p = dir / "NoOutParams.crate";
        CHECK(saveProjectFile(p.string(), ProjectInfo{"Nameless"}));
        CHECK(loadProjectFile(p.string(), nullptr));
    }

    fs::remove_all(dir, ec);
    std::printf("OK project_file_tests (%d checks)\n", g_checks);
    return 0;
}
