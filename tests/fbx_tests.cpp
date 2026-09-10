// Smoke test for the FBX importer. Loads a small binary FBX and checks that
// geometry and material data come through. Data path is injected by CMake.

#include "assets/FbxImport.h"

#include <cstdio>
#include <string>

using namespace crate;

#ifndef CRATE_TEST_DATA
#define CRATE_TEST_DATA "."
#endif

int main() {
    const std::string path = std::string(CRATE_TEST_DATA) + "/box.fbx";
    MeshLibrary lib;
    FbxImportResult r = importFbx(path, lib);

    if (!r.ok) {
        std::printf("FAIL import: %s\n", r.error.c_str());
        return 1;
    }
    const MeshData* mesh = lib.findMesh(r.key);
    if (!mesh || mesh->vertices.empty() || mesh->indices.empty()) {
        std::printf("FAIL: no geometry (%zu verts)\n", mesh ? mesh->vertices.size() : 0);
        return 1;
    }
    if (mesh->indices.size() % 3 != 0) {
        std::printf("FAIL: index count not triangulated (%zu)\n", mesh->indices.size());
        return 1;
    }
    // A cube: 12 triangles minimum after triangulation.
    if (r.triangles < 12) {
        std::printf("FAIL: expected >=12 tris, got %d\n", r.triangles);
        return 1;
    }
    // Re-import must be idempotent.
    FbxImportResult r2 = importFbx(path, lib);
    if (!r2.ok || lib.findMesh(r2.key)->vertices.size() != mesh->vertices.size()) {
        std::printf("FAIL: re-import changed vertex count\n");
        return 1;
    }

    std::printf("ok  box.fbx: %d mesh nodes, %d tris, %d materials\n", r.meshNodes, r.triangles,
                r.materials);
    return 0;
}
