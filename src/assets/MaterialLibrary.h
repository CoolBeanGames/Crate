#pragma once
#include "assets/MeshLibrary.h" // Material

#include <map>
#include <string>
#include <vector>

namespace crate {

// Named collection of Material assets. MeshRenderer::materialRef holds a name
// from here; an empty / unknown ref means "use the MeshRenderer's own colour".
class MaterialLibrary {
public:
    // Create a material, making the name unique if it already exists.
    Material& create(std::string name = "Material") {
        std::string unique = name;
        int n = 1;
        while (mats_.count(unique))
            unique = name + " " + std::to_string(++n);
        Material m;
        m.name = unique;
        return mats_.emplace(unique, std::move(m)).first->second;
    }

    Material* find(const std::string& name) {
        auto it = mats_.find(name);
        return it == mats_.end() ? nullptr : &it->second;
    }
    const Material* find(const std::string& name) const {
        auto it = mats_.find(name);
        return it == mats_.end() ? nullptr : &it->second;
    }

    void set(const std::string& name, Material m) { mats_[name] = std::move(m); }
    bool remove(const std::string& name) { return mats_.erase(name) != 0; }

    // Rename a material. Fails if the new name is taken or the old one is
    // missing. Callers must fix up MeshRenderer::materialRef.
    bool rename(const std::string& oldName, const std::string& newName) {
        if (oldName == newName || !mats_.count(oldName) || mats_.count(newName))
            return false;
        Material m = std::move(mats_[oldName]);
        m.name = newName;
        mats_.erase(oldName);
        mats_[newName] = std::move(m);
        return true;
    }

    std::vector<std::string> names() const {
        std::vector<std::string> v;
        v.reserve(mats_.size());
        for (const auto& [k, _] : mats_)
            v.push_back(k);
        return v;
    }
    bool empty() const { return mats_.empty(); }

private:
    std::map<std::string, Material> mats_;
};

} // namespace crate
