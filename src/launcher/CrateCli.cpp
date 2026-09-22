// `crate` -- a small console CLI (task 125): `crate new "path"` / `crate
// open "path"`, so a project can be created or launched without touching the
// launcher UI. Registered on PATH by the launcher (see PathRegistry.h);
// finds the current Crate.exe the same way the launcher does (a pointer file
// under %LOCALAPPDATA%\Crate, refreshed every time Crate.exe itself runs --
// never a hardcoded path).

#include "editor/PathRegistry.h"
#include "editor/ProjectFile.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

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

int printUsage() {
    std::cerr << "usage: crate new \"path/to/folder\"\n"
                 "       crate open \"path/to/folder\"\n";
    return 1;
}

// Launches Crate.exe --project <crateFilePath> and returns without waiting
// for the editor to close (same behavior as the launcher's own "Launch").
int launchEditorOn(const std::string& crateFilePath) {
    std::string editorPath = crate::readExePointer("editor_path.txt");
    if (editorPath.empty()) {
        std::cerr << "crate: Crate.exe hasn't run on this machine yet -- open it (or the "
                     "launcher) at least once first.\n";
        return 1;
    }
    std::wstring cmd = L"\"" + utf8ToWide(editorPath) + L"\" --project \"" +
                       utf8ToWide(crateFilePath) + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                          &pi)) {
        std::cerr << "crate: failed to launch '" << editorPath << "'\n";
        return 1;
    }
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return 0;
}

int cmdNew(const std::string& folder) {
    fs::path dir(folder);
    // A trailing slash (very likely typed at a CLI, e.g. "MyGame/") leaves
    // filename() empty -- fall back to the last non-empty path component.
    std::string projectName = dir.filename().string();
    if (projectName.empty())
        projectName = dir.parent_path().filename().string();
    if (projectName.empty()) {
        std::cerr << "crate: couldn't derive a project name from '" << folder << "'\n";
        return 1;
    }

    crate::CreateProjectResult result = crate::createProjectInFolder(dir.generic_string(), projectName);
    if (!result.ok) {
        std::cerr << "crate new: " << result.error << "\n";
        return 1;
    }
    crate::registerKnownProject(result.crateFilePath); // so the launcher lists it too
    std::cout << "Created project '" << projectName << "' at " << result.crateFilePath << "\n";
    return 0;
}

int cmdOpen(const std::string& folder) {
    std::string error;
    std::string crateFile = crate::findCrateFileInFolder(folder, &error);
    if (crateFile.empty()) {
        std::cerr << "crate open: " << error << "\n";
        return 1;
    }
    crate::registerKnownProject(crateFile); // so the launcher lists it too
    return launchEditorOn(crateFile);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3)
        return printUsage();
    std::string verb = argv[1];
    std::string path = argv[2];
    if (verb == "new")
        return cmdNew(path);
    if (verb == "open")
        return cmdOpen(path);
    return printUsage();
}
