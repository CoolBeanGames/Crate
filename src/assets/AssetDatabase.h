#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace crate {

// Stable identifiers for assets. Every asset (material, imported mesh/texture,
// script) is given a UUID the first time it is seen; that UUID is how the rest
// of the engine should refer to the asset, because it survives renames and
// moves. The path<->uuid mapping is persisted next to the project, in
// <projectDir>/.assetdb (tab-separated: uuid <TAB> path).
class AssetDatabase {
public:
    static AssetDatabase& get();

    // Point the database at a project and load its .assetdb (if present).
    void load(const std::string& projectDir);
    void save() const;

    // The asset's UUID, minting + persisting a fresh one if the path is new.
    const std::string& idFor(const std::string& path);

    // Look-ups; return "" when unknown.
    std::string pathOf(const std::string& uuid) const;
    std::string idOf(const std::string& path) const;

    // The asset formerly at `from` now lives at `to` (rename or move). The
    // UUID is preserved. No-op if `from` was not tracked.
    void moved(const std::string& from, const std::string& to);

    // A folder moved: repoint every tracked path that starts with `fromPrefix`
    // so that prefix becomes `toPrefix`.
    void movedPrefix(const std::string& fromPrefix, const std::string& toPrefix);

    // Drop an asset (deleted from disk).
    void forget(const std::string& path);

    struct Entry {
        std::string uuid;
        std::string path;
    };
    std::vector<Entry> entries() const;

private:
    static std::string mint();

    std::unordered_map<std::string, std::string> pathToId_;
    std::unordered_map<std::string, std::string> idToPath_;
    std::string dbFile_;
};

} // namespace crate
