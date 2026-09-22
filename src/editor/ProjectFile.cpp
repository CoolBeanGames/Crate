#include "editor/ProjectFile.h"
#include "input/InputMap.h"

#include <filesystem>
#include <fstream>

namespace crate {

namespace fs = std::filesystem;

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

CreateProjectResult createProjectInFolder(const std::string& projectDir, const std::string& projectName) {
    CreateProjectResult result;
    fs::path dir(projectDir);
    std::error_code ec;

    // Don't silently reuse/clobber a folder that already holds something --
    // an existing EMPTY folder (e.g. one the user pre-made out of habit) is
    // fine to build into, but any existing content there is someone else's,
    // not this new project's to overwrite.
    if (fs::exists(dir, ec)) {
        std::error_code ec2;
        bool nonEmpty = fs::directory_iterator(dir, ec2) != fs::directory_iterator();
        if (nonEmpty) {
            result.error = "'" + dir.generic_string() + "' already exists and is not empty";
            return result;
        }
    }

    fs::path assetsDir = dir / "assets";
    fs::create_directories(assetsDir, ec);
    if (ec) {
        result.error = "could not create '" + assetsDir.generic_string() + "'";
        return result;
    }

    // A fresh project should have a real physical default asset from the
    // start (task 124), not just in-memory state -- matches the file this
    // same InputMap::save() format already produces via the "Create Input
    // Map" asset-browser action, and the filename this repo's own
    // long-standing default project has always used.
    InputMap defaultInputMap;
    defaultInputMap.buttons.push_back({"jump", 0});
    defaultInputMap.save((assetsDir / "Default.inputmap").generic_string());

    fs::path crateFile = dir / (projectName + ".crate");
    ProjectInfo info;
    info.name = projectName;
    if (!saveProjectFile(crateFile.generic_string(), info, &result.error))
        return result;

    result.ok = true;
    result.crateFilePath = crateFile.generic_string();
    result.assetsDir = assetsDir.generic_string();
    return result;
}

std::string findCrateFileInFolder(const std::string& folder, std::string* errorOut) {
    std::error_code ec;
    fs::path found;
    int count = 0;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".crate") {
            found = entry.path();
            ++count;
        }
    }
    if (ec) {
        if (errorOut)
            *errorOut = "could not read '" + folder + "': " + ec.message();
        return {};
    }
    if (count == 0) {
        if (errorOut)
            *errorOut = "no .crate file found in '" + folder + "'";
        return {};
    }
    if (count > 1) {
        if (errorOut)
            *errorOut = "more than one .crate file in '" + folder + "' -- open the specific file instead";
        return {};
    }
    return found.generic_string();
}

} // namespace crate
