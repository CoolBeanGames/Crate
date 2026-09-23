#pragma once
#include <string>

namespace crate::platform {

// Native "open file" dialog. `filterSpec` is the Win32 double-NUL filter string,
// e.g. "Images\0*.png;*.jpg;*.tif\0All\0*.*\0". Returns an empty string if the
// user cancels or the dialog is unavailable.
std::string openFileDialog(const char* title, const char* filterSpec);

// Native "save file" dialog. `defaultExt` (no dot, e.g. "cscene") is
// appended when the user doesn't type one. Returns an empty string if the
// user cancels or the dialog is unavailable.
std::string saveFileDialog(const char* title, const char* filterSpec, const char* defaultExt);

// Opens `path` in whatever the OS default application for its file type is
// (task 139, e.g. double-clicking an image asset's icon). Fire-and-forget;
// failures just don't open anything (there's no dialog result to report).
void openWithDefaultApp(const std::string& path);

} // namespace crate::platform
