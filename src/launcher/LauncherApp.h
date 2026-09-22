#pragma once
#include <string>
#include <vector>

namespace crate {

// The Projects Launcher (task 125): a separate, deliberately small executable
// for creating/opening/managing Crate projects without going through the
// main editor. Reuses ProjectFile.h/PathRegistry.h from crate_core -- no
// project-creation logic is duplicated here, it's the exact same function
// EditorApp::newProject() and the `crate` CLI call.
class LauncherApp {
public:
    LauncherApp();

    // Draw one frame. Call between ImGui::NewFrame() and ImGui::Render().
    void onFrame();

private:
    struct ProjectEntry {
        std::string crateFilePath;
        std::string displayName;
        bool missing = false; // the .crate file no longer exists on disk
    };

    void loadKnownProjects();
    void saveKnownProjects() const;
    void refreshDisplayInfo(ProjectEntry& e) const;
    void addKnownProject(const std::string& crateFilePath); // de-dupes, saves, re-sorts

    void launchProject(const std::string& crateFilePath) const;
    void doNewProject();
    void doOpenExisting();
    void doOpenFolder(const std::string& crateFilePath) const;

    std::vector<ProjectEntry> projects_;
    int pendingDeleteIndex_ = -1; // >= 0 while the confirm-delete popup is open
    std::string statusMessage_;
    std::string knownProjectsPath_;
};

} // namespace crate
