#pragma once
#include "core/Math.h"
#include "input/InputMap.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace crate {

// Runtime input state, driven by an InputMap. poll() once per frame reads the
// keyboard (ImGui key state) and gamepad (XInput) and updates every binding.
class Input {
public:
    static Input& get();

    void setMap(const InputMap* map) { map_ = map; }
    const InputMap* map() const { return map_; }

    void poll(); // call once per frame while playing

    bool pressed(const std::string& name) const;
    bool justPressed(const std::string& name) const;
    bool justReleased(const std::string& name) const;
    // 2-button axis -> {x, 0}; 4-button / analog axis -> {x, y}.
    Vec3 axis(const std::string& name) const;

    // Transitions this frame, for the scripting signal layer to dispatch.
    // kind: 0 = pressed (held), 1 = just_pressed, 2 = just_released.
    struct Event {
        std::string button;
        int kind;
    };
    const std::vector<Event>& events() const { return events_; }

private:
    struct BtnState {
        bool down = false;
        bool prev = false;
    };
    const InputMap* map_ = nullptr;
    std::unordered_map<std::string, BtnState> buttons_;
    std::vector<Event> events_;

    static bool keyDown(int imguiKey);
    Vec3 stickValue(int stick) const;
};

} // namespace crate
