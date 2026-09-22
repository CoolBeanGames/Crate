#pragma once
#include <string>

namespace crate {

// A small, deliberately un-fancy mechanism so a separately-launched `crate`
// CLI tool (task 125) can always find the CURRENT Crate.exe, no matter which
// build directory it was last built into.
//
// Lesson learned the hard way elsewhere in this project's own tooling: a
// PATH-registered wrapper that hardcodes one absolute path to a specific
// build output folder goes silently, confusingly stale the moment that
// folder is deleted or rebuilt somewhere else -- with no way to tell it
// apart from "the tool is just broken" until someone manually tracks it
// down. The fix here is that nothing is ever hardcoded: `Crate.exe` (and the
// launcher) each call writeSelfPathPointer() once at startup, overwriting a
// tiny pointer file under %LOCALAPPDATA%\Crate every time they run. Anything
// that needs to find the editor later (readExePointer) just reads that file
// fresh -- as long as the editor has run at least once recently, the pointer
// is correct, and it never needs to change once installed.

// %LOCALAPPDATA%\Crate -- created if it doesn't exist yet.
std::string crateAppDataDir();

// Overwrites %LOCALAPPDATA%\Crate\<pointerFileName> with this process's own
// full exe path (GetModuleFileNameW). Call once at startup.
// `pointerFileName` is a plain filename, e.g. "editor_path.txt".
void writeSelfPathPointer(const char* pointerFileName);

// Reads back a pointer file written by writeSelfPathPointer(). Returns empty
// if it doesn't exist yet (the corresponding exe has never run on this
// machine) or points at a file that no longer exists on disk.
std::string readExePointer(const char* pointerFileName);

// Copies `sourceExePath` (expected to sit next to the calling process, e.g.
// "crate.exe" built alongside the launcher) into a fixed, never-renamed
// directory (%LOCALAPPDATA%\Crate\bin) and makes sure that directory is on
// the current user's PATH (HKCU\Environment), adding it if missing and
// broadcasting WM_SETTINGCHANGE so freshly-launched processes pick it up --
// an already-open terminal still needs to be reopened, same as any other
// Windows PATH change. Safe to call every time the launcher starts: it just
// re-copies the exe (so PATH itself is only ever touched once, but the
// binary living there is always refreshed to the latest build) and no-ops
// the registry write if the directory is already listed.
// Returns the stable directory the CLI now lives in (for logging), or empty
// on failure.
std::string ensureCrateCliOnPath(const std::string& sourceExePath);

// Appends `crateFilePath` to the shared known-projects list
// (%LOCALAPPDATA%\Crate\projects.txt) the launcher reads, de-duping against
// what's already there. So a project made via `crate new`/`crate open`
// shows up in the launcher too, not just ones made through the launcher's
// own UI -- "list every project you've created" (task 125) means every way
// of creating one, not just this particular entry point.
void registerKnownProject(const std::string& crateFilePath);

// Records the project + scene the editor currently has open
// (%LOCALAPPDATA%\Crate\last_opened.txt), so a bare launch of Crate.exe (no
// --project arg) can reopen exactly where the user left off instead of
// always falling back to the built-in sample scene (task 130). `scenePath`
// may be empty (e.g. a brand-new, never-saved scene).
void writeLastOpened(const std::string& projectCratePath, const std::string& scenePath);

// Fills *outProject/*outScene from the last writeLastOpened() call, leaving
// them untouched if there's no record yet or the recorded project file no
// longer exists on disk (the scene path is not existence-checked here --
// callers should verify it themselves before loading).
void readLastOpened(std::string* outProject, std::string* outScene);

} // namespace crate
