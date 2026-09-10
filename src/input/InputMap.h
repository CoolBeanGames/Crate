#pragma once
#include <string>
#include <vector>

namespace crate {

// An Input Map asset: named bindings from physical inputs (keyboard / gamepad
// buttons, or a gamepad stick) to logical action names the game queries by
// string. Saved as a small text file (assets/*.inputmap).
struct InputMap {
    // Key codes are ImGuiKey values (incl. ImGuiKey_Gamepad*). 0 = unbound.
    struct Button {
        std::string name;
        int key = 0;
    };
    struct Axis2 { // one string -> float in [-1, 1]
        std::string name;
        int negKey = 0;
        int posKey = 0;
    };
    struct Axis4 { // one string -> Vector2, each component in [-1, 1]
        std::string name;
        int leftKey = 0, rightKey = 0, downKey = 0, upKey = 0;
    };
    struct Analog { // gamepad stick -> Vector2
        std::string name;
        int stick = 0; // 0 = left stick, 1 = right stick
    };

    std::vector<Button> buttons;
    std::vector<Axis2> axes2;
    std::vector<Axis4> axes4;
    std::vector<Analog> analogs;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

} // namespace crate
