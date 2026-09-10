#include "platform/FileDialog.h"

#include <windows.h>
#include <commdlg.h>

#include <vector>

namespace crate::platform {

std::string openFileDialog(const char* title, const char* filterSpec) {
    // Convert the ANSI filter (with embedded NULs) to wide chars.
    std::vector<wchar_t> filter;
    for (const char* p = filterSpec;; ++p) {
        filter.push_back(static_cast<wchar_t>(*p));
        if (*p == '\0' && *(p + 1) == '\0') {
            filter.push_back(L'\0');
            break;
        }
    }

    wchar_t wtitle[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 255);

    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter.data();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn))
        return {};

    int len = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, file, -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace crate::platform
