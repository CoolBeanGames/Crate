#pragma once
#include "assets/MaterialLibrary.h"
#include "assets/MeshLibrary.h"
#include <string>
#include <vector>

namespace crate {

struct FbxImportResult {
    bool ok = false;
    std::string error;
    std::string key;      // MeshLibrary key for the imported geometry (= path)
    int meshNodes = 0;
    int triangles = 0;
    int materials = 0;
    std::vector<std::string> materialNames; // names registered in MaterialLibrary
};

// Load an FBX file, merge its geometry into one MeshData (registered in
// `meshes` under the file path), and create a MaterialLibrary entry for each
// FBX material (base colour + albedo texture). Safe to re-import.
FbxImportResult importFbx(const std::string& path, MeshLibrary& meshes, MaterialLibrary& materials);

} // namespace crate
