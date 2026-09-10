#include "input/InputMap.h"

#include <fstream>
#include <sstream>

namespace crate {

// Format (one binding per line):
//   button <name> <key>
//   axis2  <name> <negKey> <posKey>
//   axis4  <name> <leftKey> <rightKey> <downKey> <upKey>
//   analog <name> <stick>

bool InputMap::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    buttons.clear();
    axes2.clear();
    axes4.clear();
    analogs.clear();
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;
        if (kind == "button") {
            Button b;
            ls >> b.name >> b.key;
            if (!b.name.empty())
                buttons.push_back(b);
        } else if (kind == "axis2") {
            Axis2 a;
            ls >> a.name >> a.negKey >> a.posKey;
            if (!a.name.empty())
                axes2.push_back(a);
        } else if (kind == "axis4") {
            Axis4 a;
            ls >> a.name >> a.leftKey >> a.rightKey >> a.downKey >> a.upKey;
            if (!a.name.empty())
                axes4.push_back(a);
        } else if (kind == "analog") {
            Analog a;
            ls >> a.name >> a.stick;
            if (!a.name.empty())
                analogs.push_back(a);
        }
    }
    return true;
}

bool InputMap::save(const std::string& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    for (const auto& b : buttons)
        out << "button " << b.name << ' ' << b.key << '\n';
    for (const auto& a : axes2)
        out << "axis2 " << a.name << ' ' << a.negKey << ' ' << a.posKey << '\n';
    for (const auto& a : axes4)
        out << "axis4 " << a.name << ' ' << a.leftKey << ' ' << a.rightKey << ' ' << a.downKey << ' '
            << a.upKey << '\n';
    for (const auto& a : analogs)
        out << "analog " << a.name << ' ' << a.stick << '\n';
    return true;
}

} // namespace crate
