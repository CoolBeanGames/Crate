#include "launcher/LauncherApp.h"

#include "core/Log.h"
#include "editor/PathRegistry.h"
#include "editor/ProjectFile.h"
#include "platform/FileDialog.h"

#include "imgui.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace crate {

namespace fs = std::filesystem;

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty())
        return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1)
        return {};
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

} // namespace

LauncherApp::LauncherApp() {
    knownProjectsPath_ = (fs::path(crateAppDataDir()) / "projects.txt").generic_string();
    loadKnownProjects();

    // Register the `crate` CLI on PATH every time the launcher runs (task
    // 125) -- see PathRegistry.h for why this is safe to do unconditionally
    // and doesn't repeat the stale-hardcoded-path mistake made elsewhere in
    // this project's own tooling. crate.exe is built alongside the launcher.
    wchar_t selfPathW[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(nullptr, selfPathW, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        // crate_cli builds into build/cli-tool/crate.exe, NOT next to the
        // launcher -- see the CMakeLists.txt comment on that target for why
        // (Windows filenames are case-insensitive, so "crate.exe" next to
        // "Crate.exe" would be the same file on disk).
        fs::path cratesCliPath =
            fs::path(selfPathW).parent_path() / "cli-tool" / "crate.exe";
        std::string binDir = ensureCrateCliOnPath(cratesCliPath.generic_string());
        if (!binDir.empty())
            CR_LOG("launcher", "crate CLI available at " + binDir + "\\crate.exe");
        else
            CR_WARN("launcher", "Could not register the crate CLI on PATH (crate.exe not found "
                                "next to the launcher, or PATH could not be updated)");
    }
}

void LauncherApp::loadKnownProjects() {
    projects_.clear();
    std::ifstream in(knownProjectsPath_, std::ios::binary);
    if (!in)
        return;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        ProjectEntry e;
        e.crateFilePath = line;
        refreshDisplayInfo(e);
        projects_.push_back(std::move(e));
    }
}

void LauncherApp::saveKnownProjects() const {
    std::ofstream out(knownProjectsPath_, std::ios::binary | std::ios::trunc);
    for (const auto& e : projects_)
        out << e.crateFilePath << "\n";
}

void LauncherApp::refreshDisplayInfo(ProjectEntry& e) const {
    std::error_code ec;
    e.missing = !fs::exists(e.crateFilePath, ec);
    ProjectInfo info;
    if (!e.missing && loadProjectFile(e.crateFilePath, &info) && !info.name.empty())
        e.displayName = info.name;
    else
        e.displayName = fs::path(e.crateFilePath).stem().string();
}

void LauncherApp::addKnownProject(const std::string& crateFilePath) {
    registerKnownProject(crateFilePath); // shared with the editor's own New/Open Project + the CLI
    auto it = std::find_if(projects_.begin(), projects_.end(), [&](const ProjectEntry& e) {
        return e.crateFilePath == crateFilePath;
    });
    if (it == projects_.end()) {
        ProjectEntry e;
        e.crateFilePath = crateFilePath;
        refreshDisplayInfo(e);
        projects_.push_back(std::move(e));
    } else {
        refreshDisplayInfo(*it);
    }
}

void LauncherApp::launchProject(const std::string& crateFilePath) const {
    std::string editorPath = readExePointer("editor_path.txt");
    if (editorPath.empty()) {
        CR_ERROR("launcher", "Can't find Crate.exe -- it needs to have run at least once on this "
                             "machine before the launcher can start it");
        return;
    }
    std::wstring cmd = L"\"" + utf8ToWide(editorPath) + L"\" --project \"" +
                       utf8ToWide(crateFilePath) + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                         &pi)) {
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
    } else {
        CR_ERROR("launcher", "Failed to launch '" + editorPath + "'");
    }
}

void LauncherApp::doNewProject() {
    std::string path =
        platform::saveFileDialog("New Project", "Crate Project\0*.crate\0All\0*.*\0", "crate");
    if (path.empty())
        return;
    fs::path chosen(path);
    std::string projectName = chosen.stem().string();
    fs::path projectDir = chosen.parent_path() / projectName;

    CreateProjectResult result = createProjectInFolder(projectDir.generic_string(), projectName);
    if (!result.ok) {
        statusMessage_ = "New Project failed: " + result.error;
        CR_ERROR("launcher", statusMessage_);
        return;
    }
    addKnownProject(result.crateFilePath);
    statusMessage_ = "Created '" + projectName + "'";
}

void LauncherApp::doOpenExisting() {
    std::string path =
        platform::openFileDialog("Open Project", "Crate Project\0*.crate\0All\0*.*\0");
    if (path.empty())
        return;
    addKnownProject(path);
    statusMessage_ = "Added '" + fs::path(path).stem().string() + "'";
}

void LauncherApp::doOpenFolder(const std::string& crateFilePath) const {
    fs::path folder = fs::path(crateFilePath).parent_path();
    ::ShellExecuteW(nullptr, L"explore", utf8ToWide(folder.generic_string()).c_str(), nullptr,
                    nullptr, SW_SHOWNORMAL);
}

void LauncherApp::onFrame() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Crate Projects", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::Text("Crate Projects");
    ImGui::SameLine(ImGui::GetWindowWidth() - 260);
    if (ImGui::Button("New Project...", ImVec2(120, 0)))
        doNewProject();
    ImGui::SameLine();
    if (ImGui::Button("Open Existing...", ImVec2(120, 0)))
        doOpenExisting();
    ImGui::Separator();

    if (projects_.empty()) {
        ImGui::TextDisabled("No projects yet -- click New Project or Open Existing above.");
    } else if (ImGui::BeginTable("projects", 4,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Project");
        ImGui::TableSetupColumn("Path");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 260);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(projects_.size()); ++i) {
            ProjectEntry& e = projects_[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (e.missing)
                ImGui::TextDisabled("%s (missing)", e.displayName.c_str());
            else
                ImGui::Text("%s", e.displayName.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", e.crateFilePath.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::BeginDisabled(e.missing);
            if (ImGui::Button("Launch")) launchProject(e.crateFilePath);
            ImGui::SameLine();
            if (ImGui::Button("Open Folder")) doOpenFolder(e.crateFilePath);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                pendingDeleteIndex_ = i;
                ImGui::OpenPopup("Delete Project");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::BeginPopupModal("Delete Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (pendingDeleteIndex_ >= 0 && pendingDeleteIndex_ < static_cast<int>(projects_.size())) {
            const ProjectEntry& e = projects_[pendingDeleteIndex_];
            ImGui::Text("Remove '%s' from the project list?", e.displayName.c_str());
            ImGui::TextDisabled("%s", e.crateFilePath.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Remove from list only")) {
                projects_.erase(projects_.begin() + pendingDeleteIndex_);
                saveKnownProjects();
                pendingDeleteIndex_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove + delete files from disk")) {
                std::error_code ec;
                fs::remove_all(fs::path(e.crateFilePath).parent_path(), ec);
                projects_.erase(projects_.begin() + pendingDeleteIndex_);
                saveKnownProjects();
                pendingDeleteIndex_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                pendingDeleteIndex_ = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (!statusMessage_.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", statusMessage_.c_str());
    }

    ImGui::End();
}

} // namespace crate
