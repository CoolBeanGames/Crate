#pragma once
#include "render/Mesh.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace crate {

// A material asset: created in the asset window, edited in the inspector, and
// referenced by any MeshRenderer. FBX import creates one per FBX material.
struct Material {
    std::string name = "Material";
    float baseColor[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    std::string texturePath; // resolved to a loadable path when possible
    float emissive = 0.0f;   // adds to the lit result (0..1+)
    bool unlit = false;      // ignore scene lighting (flat/PSX look)
};

// Runtime store of imported geometry and materials, keyed by source path.
// MeshActor::meshPath is the key; an empty key means "use the primitive".
class MeshLibrary {
public:
    const MeshData* findMesh(const std::string& key) const {
        auto it = meshes_.find(key);
        return it == meshes_.end() ? nullptr : &it->second;
    }
    void addMesh(const std::string& key, MeshData data) { meshes_[key] = std::move(data); }

    bool has(const std::string& key) const { return meshes_.count(key) != 0; }

private:
    std::unordered_map<std::string, MeshData> meshes_;
};

} // namespace crate
