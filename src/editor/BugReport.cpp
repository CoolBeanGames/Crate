#include "editor/BugReport.h"

#include "core/Log.h"
#include "editor/PathRegistry.h"

#include <windows.h>
#include <shlwapi.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

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

// Standard Windows command-line argument quoting (matches CommandLineToArgvW's
// own unquoting rules) -- a plain "wrap in quotes" isn't safe once an argument
// (a bug title/description typed by hand) can itself contain a `"` or end in
// a run of backslashes.
std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(*it);
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

fs::path crateAppDataPath() { return fs::path(crateAppDataDir()); }

fs::path pendingCrashMarkerPath() { return crateAppDataPath() / "pending_crash.txt"; }

} // namespace

std::string findEngineRepoRoot() {
    std::string exePath = currentExePath();
    if (exePath.empty())
        return {};
    std::error_code ec;
    fs::path dir = fs::path(exePath).parent_path();
    for (int i = 0; i < 6 && !dir.empty(); ++i) {
        if (fs::exists(dir / "zen.tasks.json", ec))
            return dir.generic_string();
        fs::path parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = parent;
    }
    return {};
}

std::string findZenOperatorExe() {
    // PATH first (SearchPathW: the standard "does this resolve" check --
    // doesn't launch anything, unlike a trial CreateProcess).
    wchar_t found[MAX_PATH];
    if (::SearchPathW(nullptr, L"zen-operator.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        std::error_code ec;
        std::string p = wideToUtf8(found);
        if (!p.empty() && fs::exists(p, ec))
            return p;
    }
    // Fall back to the pointer file Zen itself keeps up to date -- see the
    // canonical agent instructions' own note about a shell's PATH predating
    // Zen's last update.
    if (const wchar_t* env = _wgetenv(L"LOCALAPPDATA")) {
        fs::path pointerFile = fs::path(env) / "Zen" / "operator-path.txt";
        std::ifstream in(pointerFile, std::ios::binary);
        if (in) {
            std::string path;
            std::getline(in, path);
            std::error_code ec;
            if (!path.empty() && fs::exists(path, ec))
                return path;
        }
    }
    return {};
}

BugReportResult submitBugReport(const std::string& title, const std::string& description,
                                const std::string& type) {
    BugReportResult result;

    std::string opExe = findZenOperatorExe();
    if (opExe.empty()) {
        result.message = "Couldn't find zen-operator.exe (not on PATH, and no "
                         "%LOCALAPPDATA%\\Zen\\operator-path.txt) -- is Zen installed?";
        return result;
    }
    std::string repoRoot = findEngineRepoRoot();
    if (repoRoot.empty()) {
        result.message = "Couldn't find the engine's own zen.tasks.json above this exe's "
                         "own folder -- is this a normal build (build/ or build-Release/ "
                         "inside the repo)?";
        return result;
    }
    if (title.empty()) {
        result.message = "Title is required.";
        return result;
    }

    std::wstring cmd = quoteArg(utf8ToWide(opExe));
    cmd += L" bug --branch main --title " + quoteArg(utf8ToWide(title));
    cmd += L" --task " + quoteArg(utf8ToWide(description));
    cmd += L" --tag bug";
    if (!type.empty())
        cmd += L" --tag " + quoteArg(utf8ToWide(type));

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr;
    if (!::CreatePipe(&outRead, &outWrite, &sa, 0)) {
        result.message = "Failed to set up the operator process (CreatePipe).";
        return result;
    }
    ::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};

    std::wstring workDir = utf8ToWide(repoRoot);
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    BOOL ok = ::CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                               nullptr, workDir.c_str(), &si, &pi);
    ::CloseHandle(outWrite);
    if (!ok) {
        ::CloseHandle(outRead);
        result.message = "Failed to launch zen-operator.exe (CreateProcess error " +
                         std::to_string(::GetLastError()) + ").";
        return result;
    }

    std::string output;
    char buf[4096];
    DWORD n = 0;
    while (::ReadFile(outRead, buf, sizeof(buf), &n, nullptr) && n > 0)
        output.append(buf, n);
    ::CloseHandle(outRead);

    ::WaitForSingleObject(pi.hProcess, 10000);
    DWORD exitCode = 1;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);

    // Trim trailing newline for a tidy one-line status.
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();

    result.ok = (exitCode == 0);
    result.message = output.empty() ? (result.ok ? "Filed." : "zen-operator failed.") : output;
    return result;
}

// --- Crash handling ---------------------------------------------------------

namespace {

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    // Deliberately minimal: no std::string/heap allocation, no calls into any
    // engine system -- the process may already be in a corrupted state, so
    // this only touches fixed-size stack buffers and raw WinAPI file I/O.
    wchar_t appData[MAX_PATH];
    DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return EXCEPTION_CONTINUE_SEARCH;

    wchar_t dir[MAX_PATH];
    if (::PathCombineW(dir, appData, L"Crate") == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;
    ::CreateDirectoryW(dir, nullptr); // idempotent; ignore failure/already-exists

    wchar_t path[MAX_PATH];
    if (::PathCombineW(path, dir, L"pending_crash.txt") == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    wchar_t line[256];
    DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    int len = ::wsprintfW(line, L"0x%08X at 0x%p", code, addr);

    HANDLE f = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char utf8Line[512];
        int utf8Len = ::WideCharToMultiByte(CP_UTF8, 0, line, len, utf8Line, sizeof(utf8Line),
                                            nullptr, nullptr);
        DWORD written = 0;
        if (utf8Len > 0)
            ::WriteFile(f, utf8Line, static_cast<DWORD>(utf8Len), &written, nullptr);
        ::CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH; // still crash normally afterward
}

} // namespace

void installCrashHandler() { ::SetUnhandledExceptionFilter(crashFilter); }

bool takePendingCrashReport(std::string* outErrorCode) {
    fs::path marker = pendingCrashMarkerPath();
    std::error_code ec;
    if (!fs::exists(marker, ec))
        return false;
    std::ifstream in(marker, std::ios::binary);
    std::string line;
    if (in)
        std::getline(in, line);
    in.close();
    fs::remove(marker, ec);
    if (line.empty())
        return false;
    if (outErrorCode)
        *outErrorCode = line;
    return true;
}

} // namespace crate
