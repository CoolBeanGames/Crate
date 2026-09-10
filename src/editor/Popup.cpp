#include "editor/Popup.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

namespace crate::ui {

void Popup::label(const char* text) { ImGui::TextUnformatted(text); }

void Popup::help(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

bool Popup::inputText(const char* label, std::string* value, bool focusOnAppear) {
    if (focusOnAppear && ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    return ImGui::InputText(label, value);
}

bool Popup::inputInt(const char* label, int* value) { return ImGui::InputInt(label, value); }

bool Popup::inputFloat(const char* label, float* value, float step) {
    return ImGui::InputFloat(label, value, step);
}

bool Popup::checkbox(const char* label, bool* value) { return ImGui::Checkbox(label, value); }

bool Popup::combo(const char* label, int* index, const std::vector<std::string>& options) {
    const char* preview = (*index >= 0 && *index < (int)options.size()) ? options[*index].c_str()
                                                                       : "";
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (int i = 0; i < (int)options.size(); ++i) {
            if (ImGui::Selectable(options[i].c_str(), i == *index)) {
                *index = i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool Popup::listBox(const char* label, int* selected, const std::vector<std::string>& items,
                    bool* activated, float height) {
    if (activated)
        *activated = false;
    bool changed = false;
    ImGui::TextUnformatted(label);
    if (ImGui::BeginChild(label, ImVec2(0, height), true)) {
        for (int i = 0; i < (int)items.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(items[i].c_str(), i == *selected,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                if (*selected != i) {
                    *selected = i;
                    changed = true;
                }
                if (activated && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    *activated = true;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    return changed;
}

bool Popup::button(const char* text) { return ImGui::Button(text, ImVec2(110, 0)); }

Popup::Result Popup::draw() {
    if (openRequested_) {
        ImGui::OpenPopup(title_.c_str());
        openRequested_ = false;
        pending_ = Result::Open;
    }

    Result out = Result::Open;
    ImGui::SetNextWindowSize(size_, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        isOpen_ = true;
        pending_ = Result::Open;

        if (body_)
            body_(*this);

        if (defaultButtons_) {
            ImGui::Separator();
            if (button(okLabel_.c_str()) || ImGui::IsKeyPressed(ImGuiKey_Enter))
                pending_ = Result::Ok;
            ImGui::SameLine();
            if (button("Cancel"))
                pending_ = Result::Cancel;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            pending_ = Result::Cancel;

        if (pending_ != Result::Open) {
            out = pending_;
            isOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return out;
}

} // namespace crate::ui
