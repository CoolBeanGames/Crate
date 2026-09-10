// AssetDatabase: stable UUIDs that survive renames, moves, and reloads.

#include "assets/AssetDatabase.h"

#include <cstdio>
#include <filesystem>

using namespace crate;
namespace fs = std::filesystem;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            std::printf("FAIL: %s (line %d)\n", #cond, __LINE__);                                   \
            return 1;                                                                               \
        }                                                                                           \
    } while (0)

int main() {
    fs::path dir = fs::temp_directory_path() / "crate_assetdb_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    auto& db = AssetDatabase::get();
    db.load(dir.string());

    // Minting: same path -> same id, different path -> different id.
    std::string a = db.idFor("assets/wall.png");
    std::string b = db.idFor("material:Floor");
    CHECK(!a.empty());
    CHECK(a != b);
    CHECK(db.idFor("assets/wall.png") == a);

    // Rename keeps the UUID; look-ups follow the new path.
    db.moved("assets/wall.png", "assets/textures/brick.png");
    CHECK(db.idOf("assets/wall.png").empty());
    CHECK(db.idOf("assets/textures/brick.png") == a);
    CHECK(db.pathOf(a) == "assets/textures/brick.png");

    // Folder move: prefix repoint.
    db.idFor("assets/textures/moss.png");
    db.movedPrefix("assets/textures/", "assets/env/");
    CHECK(db.idOf("assets/env/brick.png") == a);
    CHECK(!db.idOf("assets/env/moss.png").empty());

    // Persistence: a fresh load from the same dir sees the same mapping.
    db.load(dir.string());
    CHECK(db.pathOf(a) == "assets/env/brick.png");
    CHECK(db.idOf("material:Floor") == b);

    // forget drops it.
    db.forget("assets/env/brick.png");
    CHECK(db.idOf("assets/env/brick.png").empty());
    CHECK(db.pathOf(a).empty());

    fs::remove_all(dir);
    std::printf("ok  AssetDatabase: mint, rename, folder move, persistence, forget\n");
    return 0;
}
