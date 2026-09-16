#pragma once
#include <string>
#include <unordered_set>
#include <vector>

namespace crate::editor::scriptbuild {

// Result of compiling one namespace's scripts into a native DLL (see
// transpiration.txt, "Transplation" Phase 3).
struct BuildResult {
    bool ok = false;
    std::string namespaceName;
    std::string dllPath; // versioned output DLL, valid iff ok
    std::string error;   // compiler/linker diagnostics, valid iff !ok
};

// Locates and caches the MSVC toolchain's environment (the INCLUDE/LIB/PATH
// vcvars64.bat produces) for the lifetime of the process, via vswhere.exe --
// so repeated builds don't each pay vcvars64.bat's own non-trivial startup
// cost the way scripts/build.ps1 does on every invocation. A singleton
// (construction does real subprocess work); check available() before run().
class ToolchainEnv {
public:
    static ToolchainEnv& get();

    bool available() const { return ok_; }
    const std::string& error() const { return error_; }

    // Runs `commandLine` (e.g. "cl.exe /nologo /c ...") with the cached
    // toolchain environment and the given working directory, capturing
    // combined stdout+stderr into `output`. Returns the process's exit
    // code, or -1 if it could not be launched at all (available() was
    // false, or CreateProcess itself failed).
    int run(const std::string& commandLine, const std::string& cwd, std::string& output) const;

private:
    ToolchainEnv();
    bool ok_ = false;
    std::string error_;
    std::vector<std::pair<std::string, std::string>> envVars_; // the full vcvars64.bat environment
};

// Determines which namespaces need rebuilding: a namespace's set of scripts
// or any of their source text changed (or it's never been built) since the
// last successful build recorded in <scriptsDir>/.crate_build/manifest.tsv.
std::unordered_set<std::string> computeDirtyNamespaces(const std::string& scriptsDir);

// Expands `dirty` to include any namespace that structurally depends (via
// script-to-script inheritance) on an already-dirty one, so a base-class
// change safely rebuilds every dependent namespace instead of leaving them
// linked against a stale layout. Returns the expanded set. NOTE: as of
// Phase 2, generateClass() refuses any base other than Actor/Actor2D/
// Actor3D, so no cross-namespace dependency edge can exist yet and this is
// currently always the identity function on non-empty input -- built now
// so Phase 4/5 inheritance support doesn't require revisiting this phase.
std::unordered_set<std::string> computeRebuildSet(const std::unordered_set<std::string>& dirty,
                                                  const std::string& scriptsDir);

// Compiles every script currently in `namespaceName` (looked up live from
// ScriptSystem) into a fresh, versioned, shadow-copied
// <scriptsDir>/.crate_build/bin/<namespaceName>_v<N>.dll -- linked against
// crate_script_runtime.lib, NEVER crate_core.lib (see transpiration.txt
// Phase 0). Updates the manifest on success only.
BuildResult buildNamespace(const std::string& namespaceName, const std::string& scriptsDir);

} // namespace crate::editor::scriptbuild
