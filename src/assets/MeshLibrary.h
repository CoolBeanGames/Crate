#pragma once
#include "render/Mesh.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace crate {

// A basic material record. The full material-asset workflow (create in the asset
// window, edit in the inspector) lands with the Materials task; for now this is
// what an FBX import produces and what the renderer reads.
struct Material {
    std::string name = "Material";
    float baseColor[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    std::string texturePath; // resolved to an absolute/loadable path when possible
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

    const std::vector<Material>* materialsFor(const std::string& key) const {
        auto it = materials_.find(key);
        return it == materials_.end() ? nullptr : &it->second;
    }
    void setMaterials(const std::string& key, std::vector<Material> mats) {
        materials_[key] = std::move(mats);
    }

    bool has(const std::string& key) const { return meshes_.count(key) != 0; }

private:
    std::unordered_map<std::string, MeshData> meshes_;
    std::unordered_map<std::string, std::vector<Material>> materials_;
};

} // namespace crate
