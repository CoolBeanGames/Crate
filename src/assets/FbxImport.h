#pragma once
#include "assets/MeshLibrary.h"
#include <string>

namespace crate {

struct FbxImportResult {
    bool ok = false;
    std::string error;
    std::string key;      // MeshLibrary key for the imported geometry (= path)
    int meshNodes = 0;
    int triangles = 0;
    int materials = 0;
};

// Load an FBX file, merge its geometry into one MeshData, extract its materials
// (base colour + albedo texture path), and register both in `lib` under the
// file path. Safe to call again for the same path (re-imports).
FbxImportResult importFbx(const std::string& path, MeshLibrary& lib);

} // namespace crate
