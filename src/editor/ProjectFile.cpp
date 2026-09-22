#include "editor/ProjectFile.h"

#include <fstream>

namespace crate {

namespace {
constexpr int kProjectVersion = 1;
}

bool saveProjectFile(const std::string& path, const ProjectInfo& info, std::string* errorOut) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (errorOut)
            *errorOut = "Could not open '" + path + "' for writing";
        return false;
    }
    out << "CRATE_PROJECT " << kProjectVersion << "\n";
    out << "NAME " << info.name << "\n";
    return true;
}

bool loadProjectFile(const std::string& path, ProjectInfo* outInfo, std::string* errorOut) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (errorOut)
            *errorOut = "Could not open '" + path + "'";
        return false;
    }
    std::string line;
    bool sawHeader = false;
    ProjectInfo info;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("CRATE_PROJECT", 0) == 0) {
            sawHeader = true;
        } else if (line.rfind("NAME ", 0) == 0) {
            info.name = line.substr(5);
        }
    }
    if (!sawHeader) {
        if (errorOut)
            *errorOut = "'" + path + "' is not a Crate project file";
        return false;
    }
    if (outInfo)
        *outInfo = info;
    return true;
}

} // namespace crate
