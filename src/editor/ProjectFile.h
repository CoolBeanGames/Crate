#pragma once
#include <string>

namespace crate {

// A Crate Project (.crate file): the small on-disk marker that a folder is a
// project root, sitting next to that folder's "assets" subfolder (the same
// subfolder the Asset Browser has always pointed at -- opening a project just
// repoints assetDir_ there). Deliberately minimal for now (task 92: "for now
// just get this working", launcher/recent-projects list explicitly deferred).
struct ProjectInfo {
    std::string name;
};

// Hand-rolled line-oriented text format, matching this project's existing
// convention of small custom formats over a JSON dependency (see
// scene/SceneIO.cpp's header comment):
//   CRATE_PROJECT <version>   -- always first
//   NAME <name>               -- rest of line
bool saveProjectFile(const std::string& path, const ProjectInfo& info, std::string* errorOut = nullptr);
bool loadProjectFile(const std::string& path, ProjectInfo* outInfo, std::string* errorOut = nullptr);

} // namespace crate
