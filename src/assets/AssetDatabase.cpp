#include "assets/AssetDatabase.h"

#include "core/Log.h"

#include <fstream>
#include <random>

namespace crate {

AssetDatabase& AssetDatabase::get() {
    static AssetDatabase db;
    return db;
}

std::string AssetDatabase::mint() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::uniform_int_distribution<int> d(0, 15);
    // 8-4-4-4-12 hex, version-4-ish layout (good enough for local asset ids).
    std::string s;
    s.reserve(36);
    for (int i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20)
            s += '-';
        s += hex[d(rng)];
    }
    return s;
}

void AssetDatabase::load(const std::string& projectDir) {
    pathToId_.clear();
    idToPath_.clear();
    dbFile_ = projectDir + "/.assetdb";

    std::ifstream in(dbFile_, std::ios::binary);
    if (!in)
        return;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        std::string uuid = line.substr(0, tab);
        std::string path = line.substr(tab + 1);
        if (uuid.empty() || path.empty())
            continue;
        pathToId_[path] = uuid;
        idToPath_[uuid] = path;
    }
}

void AssetDatabase::save() const {
    if (dbFile_.empty())
        return;
    std::ofstream out(dbFile_, std::ios::binary | std::ios::trunc);
    if (!out) {
        CR_ERROR("assets", "Could not write asset database: " + dbFile_);
        return;
    }
    for (const auto& [path, uuid] : pathToId_)
        out << uuid << '\t' << path << '\n';
}

const std::string& AssetDatabase::idFor(const std::string& path) {
    auto it = pathToId_.find(path);
    if (it != pathToId_.end())
        return it->second;
    std::string uuid = mint();
    while (idToPath_.count(uuid))
        uuid = mint();
    auto res = pathToId_.emplace(path, std::move(uuid));
    idToPath_[res.first->second] = path;
    save();
    return res.first->second;
}

std::string AssetDatabase::pathOf(const std::string& uuid) const {
    auto it = idToPath_.find(uuid);
    return it == idToPath_.end() ? std::string{} : it->second;
}

std::string AssetDatabase::idOf(const std::string& path) const {
    auto it = pathToId_.find(path);
    return it == pathToId_.end() ? std::string{} : it->second;
}

void AssetDatabase::moved(const std::string& from, const std::string& to) {
    if (from == to)
        return;
    auto it = pathToId_.find(from);
    if (it == pathToId_.end())
        return;
    std::string uuid = it->second;
    pathToId_.erase(it);
    pathToId_[to] = uuid;
    idToPath_[uuid] = to;
    save();
}

void AssetDatabase::movedPrefix(const std::string& fromPrefix, const std::string& toPrefix) {
    if (fromPrefix == toPrefix)
        return;
    std::vector<std::pair<std::string, std::string>> repoint;
    for (const auto& [path, uuid] : pathToId_) {
        if (path.rfind(fromPrefix, 0) == 0)
            repoint.push_back({path, toPrefix + path.substr(fromPrefix.size())});
    }
    for (const auto& [oldPath, newPath] : repoint) {
        std::string uuid = pathToId_[oldPath];
        pathToId_.erase(oldPath);
        pathToId_[newPath] = uuid;
        idToPath_[uuid] = newPath;
    }
    if (!repoint.empty())
        save();
}

void AssetDatabase::forget(const std::string& path) {
    auto it = pathToId_.find(path);
    if (it == pathToId_.end())
        return;
    idToPath_.erase(it->second);
    pathToId_.erase(it);
    save();
}

std::vector<AssetDatabase::Entry> AssetDatabase::entries() const {
    std::vector<Entry> v;
    v.reserve(pathToId_.size());
    for (const auto& [path, uuid] : pathToId_)
        v.push_back({uuid, path});
    return v;
}

} // namespace crate
