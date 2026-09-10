#pragma once
#include <string>

namespace crate::platform {

// Native "open file" dialog. `filterSpec` is the Win32 double-NUL filter string,
// e.g. "Images\0*.png;*.jpg;*.tif\0All\0*.*\0". Returns an empty string if the
// user cancels or the dialog is unavailable.
std::string openFileDialog(const char* title, const char* filterSpec);

} // namespace crate::platform
