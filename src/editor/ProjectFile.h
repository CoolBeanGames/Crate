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

struct CreateProjectResult {
    bool ok = false;
    std::string crateFilePath; // <projectDir>/<projectName>.crate
    std::string assetsDir;     // <projectDir>/assets
    std::string error;
};

// Shared by EditorApp::newProject() (task 92/123), the launcher's "New
// Project", and the `crate new` CLI tool (task 125) -- one place that
// actually creates a project on disk, so all three stay identical instead of
// drifting. `projectDir` is the FINAL directory the project lives in (the
// caller decides whether that's `<pickedRoot>/<name>` for a dialog-driven
// flow or a folder the user already named directly on the CLI); `projectName`
// names the .crate file and is stored as the project's display name. Refuses
// to run inside a `projectDir` that already exists and is non-empty (an
// existing EMPTY folder, e.g. one the user pre-made out of habit, is fine).
CreateProjectResult createProjectInFolder(const std::string& projectDir, const std::string& projectName);

// Finds the single *.crate file directly inside `folder` (non-recursive).
// Returns its path, or empty with *errorOut set if there isn't exactly one
// (none found, or more than one -- ambiguous, needs a specific file instead).
std::string findCrateFileInFolder(const std::string& folder, std::string* errorOut = nullptr);

} // namespace crate
