#pragma once
#include <string>

namespace crate {

// Bug Reporting (Zen task 89): lets the person running Crate.exe file a bug
// straight into this engine's own Zen task queue (zen.tasks.json in the
// engine's source repo) without leaving the editor, and offers to do the
// same automatically the next time it starts up after a crash.

// Finds the engine's own source repo root -- the directory containing
// zen.tasks.json -- by walking up from this running exe's own path. Crate.exe
// is always built into <repoRoot>/build or <repoRoot>/build-Release, so this
// works regardless of which *project* the editor currently has open (that's
// a separate, unrelated directory the user is making a game in). Returns
// empty if no ancestor directory has zen.tasks.json (e.g. a build copied
// somewhere outside the repo).
std::string findEngineRepoRoot();

// Finds zen-operator.exe: tries the current PATH first, then falls back to
// the pointer file Zen itself keeps at %LOCALAPPDATA%\Zen\operator-path.txt
// (the same fallback the canonical agent instructions document, for when a
// shell's PATH predates Zen's last update). Returns empty if neither works.
std::string findZenOperatorExe();

struct BugReportResult {
    bool ok = false;
    std::string message; // human-readable outcome, for a status line in the UI
};

// Runs `zen-operator bug --branch main --title <title> --task <description>
// --tag bug --tag <type>` with its working directory set to the engine repo
// root, so it finds zen.tasks.json regardless of the editor's current
// project. `type` is one of the Bug Reporting window's Type choices (may be
// empty).
BugReportResult submitBugReport(const std::string& title, const std::string& description,
                                const std::string& type);

// Installs a process-wide unhandled-exception filter that -- on an actual
// crash -- does the bare minimum safe thing (no heap allocation, no calls
// into engine systems that might themselves be in a corrupted state): format
// the raw exception code/address into a fixed stack buffer and write it to a
// small marker file with plain WinAPI file I/O. Call once, early in main().
void installCrashHandler();

// Checks for a marker file left by installCrashHandler()'s filter, deleting
// it either way (so the prompt only ever appears once per crash). Returns
// true and fills *outErrorCode (e.g. "0xC0000005 at 0x00007FF6...") if the
// previous run crashed.
bool takePendingCrashReport(std::string* outErrorCode);

} // namespace crate
