#include "input/Input.h"

#include "imgui.h"

#include <windows.h>
#include <xinput.h>

namespace crate {

Input& Input::get() {
    static Input in;
    return in;
}

bool Input::keyDown(int imguiKey) {
    return imguiKey > 0 && ImGui::IsKeyDown(static_cast<ImGuiKey>(imguiKey));
}

Vec3 Input::stickValue(int stick) const {
    XINPUT_STATE st{};
    if (XInputGetState(0, &st) != ERROR_SUCCESS)
        return {0, 0, 0};
    float x = (stick == 0 ? st.Gamepad.sThumbLX : st.Gamepad.sThumbRX) / 32767.0f;
    float y = (stick == 0 ? st.Gamepad.sThumbLY : st.Gamepad.sThumbRY) / 32767.0f;
    const float dz = 0.20f;
    auto curve = [&](float v) { return std::fabs(v) < dz ? 0.0f : v; };
    return {curve(x), curve(y), 0};
}

void Input::poll() {
    events_.clear();
    if (!map_)
        return;

    for (const auto& b : map_->buttons) {
        BtnState& s = buttons_[b.name];
        s.prev = s.down;
        s.down = keyDown(b.key);
        if (s.down && !s.prev)
            events_.push_back({b.name, 1});
        else if (!s.down && s.prev)
            events_.push_back({b.name, 2});
        else if (s.down)
            events_.push_back({b.name, 0});
    }
}

bool Input::pressed(const std::string& name) const {
    auto it = buttons_.find(name);
    return it != buttons_.end() && it->second.down;
}
bool Input::justPressed(const std::string& name) const {
    auto it = buttons_.find(name);
    return it != buttons_.end() && it->second.down && !it->second.prev;
}
bool Input::justReleased(const std::string& name) const {
    auto it = buttons_.find(name);
    return it != buttons_.end() && !it->second.down && it->second.prev;
}

Vec3 Input::axis(const std::string& name) const {
    if (!map_)
        return {0, 0, 0};
    for (const auto& a : map_->axes2)
        if (a.name == name)
            return {(keyDown(a.posKey) ? 1.0f : 0.0f) - (keyDown(a.negKey) ? 1.0f : 0.0f), 0, 0};
    for (const auto& a : map_->axes4)
        if (a.name == name)
            return {(keyDown(a.rightKey) ? 1.0f : 0.0f) - (keyDown(a.leftKey) ? 1.0f : 0.0f),
                    (keyDown(a.upKey) ? 1.0f : 0.0f) - (keyDown(a.downKey) ? 1.0f : 0.0f), 0};
    for (const auto& a : map_->analogs)
        if (a.name == name)
            return stickValue(a.stick);
    return {0, 0, 0};
}

} // namespace crate
