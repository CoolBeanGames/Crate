#include "editor/PathRegistry.h"

#include <windows.h>

#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <vector>

namespace crate {

namespace fs = std::filesystem;

namespace {

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty())
        return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

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

std::string currentExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    return wideToUtf8(std::wstring(buf, n));
}

} // namespace

std::string crateAppDataDir() {
    fs::path dir;
    if (const wchar_t* env = _wgetenv(L"LOCALAPPDATA"))
        dir = fs::path(env) / "Crate";
    else
        dir = fs::path("."); // last-resort fallback; should never hit on Windows
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.generic_string();
}

void writeSelfPathPointer(const char* pointerFileName) {
    std::string exePath = currentExePath();
    if (exePath.empty())
        return;
    fs::path pointerFile = fs::path(crateAppDataDir()) / pointerFileName;
    std::ofstream out(pointerFile, std::ios::binary | std::ios::trunc);
    if (out)
        out << exePath;
}

std::string readExePointer(const char* pointerFileName) {
    fs::path pointerFile = fs::path(crateAppDataDir()) / pointerFileName;
    std::ifstream in(pointerFile, std::ios::binary);
    if (!in)
        return {};
    std::string path;
    std::getline(in, path);
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec))
        return {};
    return path;
}

std::string ensureCrateCliOnPath(const std::string& sourceExePath) {
    std::error_code ec;
    if (!fs::exists(sourceExePath, ec))
        return {};

    fs::path binDir = fs::path(crateAppDataDir()) / "bin";
    fs::create_directories(binDir, ec);
    fs::path dest = binDir / "crate.exe";
    fs::copy_file(sourceExePath, dest, fs::copy_options::overwrite_existing, ec);
    if (ec)
        return {};

    // --- Make sure binDir is on HKCU\Environment\Path -----------------------
    std::wstring binDirW = utf8ToWide(binDir.generic_string());
    // generic_string() uses forward slashes; PATH entries read fine either
    // way on Windows, but normalize to native separators for tidiness.
    for (auto& c : binDirW)
        if (c == L'/')
            c = L'\\';

    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ | KEY_WRITE, &key) !=
        ERROR_SUCCESS)
        return binDir.generic_string(); // exe is refreshed even if PATH can't be touched

    wchar_t buf[32768];
    DWORD size = sizeof(buf);
    DWORD type = REG_SZ;
    std::wstring current;
    if (::RegQueryValueExW(key, L"Path", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) ==
        ERROR_SUCCESS)
        current.assign(buf, size / sizeof(wchar_t) - 1);

    bool alreadyPresent = false;
    {
        std::wstring lowerCurrent = current, lowerBin = binDirW;
        for (auto& c : lowerCurrent) c = towlower(c);
        for (auto& c : lowerBin) c = towlower(c);
        alreadyPresent = lowerCurrent.find(lowerBin) != std::wstring::npos;
    }

    if (!alreadyPresent) {
        std::wstring updated = current.empty() ? binDirW : current + L";" + binDirW;
        ::RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ,
                         reinterpret_cast<const BYTE*>(updated.c_str()),
                         static_cast<DWORD>((updated.size() + 1) * sizeof(wchar_t)));
        // Tell already-running processes (mainly Explorer) a new PATH exists,
        // so newly-launched terminals pick it up. A terminal already open
        // when this runs still needs to be reopened -- ordinary Windows PATH
        // semantics, not something a broadcast can retroactively fix.
        DWORD_PTR result = 0;
        ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                              reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 5000,
                              &result);
    }
    ::RegCloseKey(key);
    return binDir.generic_string();
}

void registerKnownProject(const std::string& crateFilePath) {
    fs::path listPath = fs::path(crateAppDataDir()) / "projects.txt";

    std::vector<std::string> lines;
    {
        std::ifstream in(listPath, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                lines.push_back(line);
        }
    }
    for (const auto& l : lines)
        if (l == crateFilePath)
            return; // already registered

    std::ofstream out(listPath, std::ios::binary | std::ios::app);
    out << crateFilePath << "\n";
}

void writeLastOpened(const std::string& projectCratePath, const std::string& scenePath) {
    fs::path p = fs::path(crateAppDataDir()) / "last_opened.txt";
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (out)
        out << projectCratePath << "\n" << scenePath << "\n";
}

void readLastOpened(std::string* outProject, std::string* outScene) {
    fs::path p = fs::path(crateAppDataDir()) / "last_opened.txt";
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return;
    std::string proj, scene;
    std::getline(in, proj);
    std::getline(in, scene);
    std::error_code ec;
    if (proj.empty() || !fs::exists(proj, ec))
        return;
    if (outProject)
        *outProject = proj;
    if (outScene)
        *outScene = scene;
}

} // namespace crate
