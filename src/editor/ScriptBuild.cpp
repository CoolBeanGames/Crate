#include "editor/ScriptBuild.h"

#include "script/ClassInfo.h"
#include "script/CodeGen.h"
#include "script/ScriptSystem.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#ifndef CRATE_REPO_DIR
#define CRATE_REPO_DIR "."
#endif
#ifndef CRATE_ENGINE_BUILD_DIR
#define CRATE_ENGINE_BUILD_DIR "."
#endif

namespace crate::editor::scriptbuild {
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16 conversion and a CreateProcessW-based subprocess runner
// that captures combined stdout+stderr and takes an explicit environment
// block, so callers never need to shell-escape environment variable values
// (PATH/INCLUDE/LIB can contain spaces and parentheses -- "Program Files
// (x86)" -- that are awkward to pass safely through `cmd.exe`'s own `set`).
// ---------------------------------------------------------------------------

std::wstring toWide(const std::string& s) {
    if (s.empty())
        return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Runs `commandLine` (interpreted by cmd.exe, so PATH-based executable
// lookup works exactly like a normal shell invocation) with `envBlock`
// (nullptr => inherit this process's environment) and `cwd`. Returns the
// exit code, or -1 if the process could not be created.
int runProcess(const std::string& commandLine, const wchar_t* envBlock, const std::string& cwd,
              std::string& output) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return -1;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring full = L"cmd.exe /c \"" + toWide(commandLine) + L"\"";
    std::wstring cwdW = toWide(cwd);
    // CreateProcessW may write into the command-line buffer; it must not be
    // a literal/read-only string.
    std::vector<wchar_t> cmdBuf(full.begin(), full.end());
    cmdBuf.push_back(L'\0');

    BOOL created =
        CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
                       CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, (LPVOID)envBlock,
                       cwd.empty() ? nullptr : cwdW.c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        return -1;
    }

    char buf[4096];
    DWORD n = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0)
        output.append(buf, n);
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

void trimTrailingNewlines(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
}

// A stable (not std::hash -- whose algorithm is unspecified and may differ
// across standard library versions/rebuilds) 64-bit fingerprint, so a
// persisted fingerprint in manifest.tsv keeps comparing correctly across
// process restarts and engine rebuilds.
uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string manifestPath(const std::string& scriptsDir) { return scriptsDir + "/.crate_build/manifest.tsv"; }

struct ManifestEntry {
    int generation = 0;
    uint64_t fingerprint = 0;
};

std::map<std::string, ManifestEntry> readManifest(const std::string& scriptsDir) {
    std::map<std::string, ManifestEntry> m;
    std::ifstream in(manifestPath(scriptsDir), std::ios::binary);
    if (!in)
        return m;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::istringstream iss(line);
        std::string ns, genStr, fpStr;
        if (!std::getline(iss, ns, '\t') || !std::getline(iss, genStr, '\t') ||
            !std::getline(iss, fpStr, '\t'))
            continue;
        if (ns.empty())
            continue;
        ManifestEntry e;
        try {
            e.generation = std::stoi(genStr);
            e.fingerprint = std::stoull(fpStr);
        } catch (...) {
            continue;
        }
        m[ns] = e;
    }
    return m;
}

void writeManifest(const std::string& scriptsDir, const std::map<std::string, ManifestEntry>& m) {
    std::error_code ec;
    fs::create_directories(scriptsDir + "/.crate_build", ec);
    std::ofstream out(manifestPath(scriptsDir), std::ios::binary | std::ios::trunc);
    if (!out)
        return;
    for (const auto& [ns, e] : m)
        out << ns << '\t' << e.generation << '\t' << e.fingerprint << '\n';
}

// Groups every currently-loaded script (ScriptSystem::files(), which
// includes ones with a parse error -- their source still counts toward the
// namespace's fingerprint, since fixing the error is itself a change worth
// rebuilding for) by namespace.
std::map<std::string, std::vector<const script::ScriptSystem::ScriptFile*>> filesByNamespace() {
    auto& sys = script::ScriptSystem::get();
    std::map<std::string, std::vector<const script::ScriptSystem::ScriptFile*>> out;
    for (const auto& f : sys.files())
        out[sys.namespaceOf(f.name)].push_back(&f);
    return out;
}

uint64_t fingerprintOf(const std::vector<const script::ScriptSystem::ScriptFile*>& files) {
    std::vector<const script::ScriptSystem::ScriptFile*> sorted = files;
    std::sort(sorted.begin(), sorted.end(),
             [](const auto* a, const auto* b) { return a->name < b->name; });
    std::string blob;
    for (const auto* f : sorted) {
        blob += f->name;
        blob += '\x1f';
        blob += f->source;
        blob += '\x1e';
    }
    return fnv1a(blob);
}

#if defined(NDEBUG)
constexpr const char* kCrtFlag = "/MD";
constexpr const char* kConfigFlags = "/O2 /Ob2 /DNDEBUG";
#else
constexpr const char* kCrtFlag = "/MDd";
constexpr const char* kConfigFlags = "/Zi /Ob0 /Od /RTC1";
#endif

} // namespace

// ---------------------------------------------------------------------------
// ToolchainEnv
// ---------------------------------------------------------------------------

ToolchainEnv& ToolchainEnv::get() {
    static ToolchainEnv instance;
    return instance;
}

ToolchainEnv::ToolchainEnv() {
    const std::string vswhere =
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    std::error_code ec;
    if (!fs::exists(vswhere, ec)) {
        error_ = "vswhere.exe not found -- no Visual Studio installation detected";
        return;
    }

    std::string installPath;
    {
        std::string out;
        int rc = runProcess("\"" + vswhere + "\" -latest -property installationPath", nullptr, "",
                            out);
        trimTrailingNewlines(out);
        if (rc != 0 || out.empty()) {
            error_ = "vswhere.exe could not find a Visual Studio installation";
            return;
        }
        installPath = out;
    }

    const std::string vcvars = installPath + "\\VC\\Auxiliary\\Build\\vcvars64.bat";
    if (!fs::exists(vcvars, ec)) {
        error_ = "vcvars64.bat not found at " + vcvars;
        return;
    }

    std::string dump;
    // Inherit the current process's environment for this one bootstrap call
    // (envBlock = nullptr) -- only the captured *output* is cached, not this
    // call's own environment.
    int rc = runProcess("\"" + vcvars + "\" >nul && set", nullptr, "", dump);
    if (rc != 0) {
        error_ = "vcvars64.bat failed to run";
        return;
    }

    std::istringstream iss(dump);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;
        envVars_.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    if (envVars_.empty()) {
        error_ = "vcvars64.bat produced no environment output";
        return;
    }
    ok_ = true;
}

int ToolchainEnv::run(const std::string& commandLine, const std::string& cwd,
                      std::string& output) const {
    if (!ok_) {
        output = error_;
        return -1;
    }
    std::wstring block;
    for (const auto& [k, v] : envVars_) {
        block += toWide(k);
        block += L'=';
        block += toWide(v);
        block += L'\0';
    }
    block += L'\0';
    return runProcess(commandLine, block.c_str(), cwd, output);
}

// ---------------------------------------------------------------------------
// Dirty tracking / dependency graph
// ---------------------------------------------------------------------------

std::unordered_set<std::string> computeDirtyNamespaces(const std::string& scriptsDir) {
    std::unordered_set<std::string> dirty;
    auto manifest = readManifest(scriptsDir);
    for (const auto& [ns, files] : filesByNamespace()) {
        uint64_t fp = fingerprintOf(files);
        auto it = manifest.find(ns);
        if (it == manifest.end() || it->second.fingerprint != fp)
            dirty.insert(ns);
    }
    return dirty;
}

std::unordered_set<std::string> computeRebuildSet(const std::unordered_set<std::string>& dirty,
                                                  const std::string& /*scriptsDir*/) {
    // See ScriptBuild.h: no cross-namespace dependency edges can exist while
    // generateClass() refuses every base but Actor/Actor2D/Actor3D, so the
    // transitive closure over "namespace B depends on namespace A" is
    // always empty today -- this is the identity function until Phase 4/5
    // add script-to-script inheritance support, at which point the edge
    // computation (scan each namespace's classes' ClassInfo::baseClass for
    // one resolving into a *different* namespace) slots in here without
    // needing to change any caller.
    return dirty;
}

// ---------------------------------------------------------------------------
// buildNamespace
// ---------------------------------------------------------------------------

BuildResult buildNamespace(const std::string& namespaceName, const std::string& scriptsDir) {
    BuildResult r;
    r.namespaceName = namespaceName;

    auto& tc = ToolchainEnv::get();
    if (!tc.available()) {
        r.error = "no MSVC toolchain available: " + tc.error();
        return r;
    }

    auto& sys = script::ScriptSystem::get();
    std::vector<const script::ClassInfo*> classes;
    for (const auto& [name, ci] : sys.types())
        if (sys.namespaceOf(name) == namespaceName && ci->decl)
            classes.push_back(ci.get());
    if (classes.empty()) {
        r.error = "namespace '" + namespaceName + "' has no compilable scripts";
        return r;
    }
    std::sort(classes.begin(), classes.end(),
             [](const auto* a, const auto* b) { return a->name < b->name; });

    const std::string buildRoot = scriptsDir + "/.crate_build";
    const std::string genDir = buildRoot + "/gen/" + namespaceName;
    const std::string binDir = buildRoot + "/bin";
    std::error_code ec;
    fs::create_directories(genDir, ec);
    fs::create_directories(binDir, ec);

    std::vector<std::string> cppFiles;
    for (const auto* ci : classes) {
        script::CodeGenResult cg = script::generateClass(*ci->decl);
        if (!cg.ok) {
            r.error = ci->name + ": " + cg.error;
            return r;
        }
        const std::string hPath = genDir + "/" + cg.className + ".gen.h";
        const std::string cPath = genDir + "/" + cg.className + ".gen.cpp";
        {
            std::ofstream hf(hPath, std::ios::binary);
            hf << cg.header;
        }
        {
            std::ofstream cf(cPath, std::ios::binary);
            cf << cg.source;
        }
        cppFiles.push_back(cPath);
    }

    auto manifest = readManifest(scriptsDir);
    int generation = manifest.count(namespaceName) ? manifest[namespaceName].generation + 1 : 1;
    const std::string dllBase = namespaceName + "_v" + std::to_string(generation);
    const std::string dllPath = binDir + "/" + dllBase + ".dll";
    const std::string pdbPath = binDir + "/" + dllBase + ".pdb";
    const std::string libPath = binDir + "/" + dllBase + ".lib";

    const std::string incSrc = std::string(CRATE_REPO_DIR) + "\\src";
    const std::string incThirdParty = std::string(CRATE_REPO_DIR) + "\\third_party";

    std::vector<std::string> objFiles;
    for (const auto& cpp : cppFiles) {
        std::string obj = fs::path(cpp).replace_extension(".obj").string();
        std::string cmd = "cl.exe /nologo /c /std:c++17 /EHsc /DWIN32 /D_WINDOWS " +
                          std::string(kConfigFlags) + " " + kCrtFlag + " -I\"" + incSrc +
                          "\" -I\"" + incThirdParty + "\" \"" + cpp + "\" /Fo\"" + obj + "\"";
        std::string out;
        int rc = tc.run(cmd, genDir, out);
        if (rc != 0 || !fs::exists(obj, ec)) {
            r.error = "compiling " + fs::path(cpp).filename().string() + " failed:\n" + out;
            return r;
        }
        objFiles.push_back(obj);
    }

    const std::string runtimeLib = std::string(CRATE_ENGINE_BUILD_DIR) + "\\crate_script_runtime.lib";
    std::string linkCmd = "link.exe /nologo /DLL /OUT:\"" + dllPath + "\" /PDB:\"" + pdbPath +
                          "\" /IMPLIB:\"" + libPath + "\"";
    for (const auto& obj : objFiles)
        linkCmd += " \"" + obj + "\"";
    linkCmd += " \"" + runtimeLib + "\"";
    {
        std::string out;
        int rc = tc.run(linkCmd, binDir, out);
        if (rc != 0 || !fs::exists(dllPath, ec)) {
            r.error = "linking " + dllBase + ".dll failed:\n" + out;
            return r;
        }
    }

    ManifestEntry entry;
    entry.generation = generation;
    entry.fingerprint = fingerprintOf(filesByNamespace()[namespaceName]);
    manifest[namespaceName] = entry;
    writeManifest(scriptsDir, manifest);

    r.ok = true;
    r.dllPath = dllPath;
    return r;
}

} // namespace crate::editor::scriptbuild
