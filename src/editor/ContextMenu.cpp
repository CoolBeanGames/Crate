#include "editor/ContextMenu.h"

#include "imgui.h"

namespace crate::ui {

bool ContextMenu::beginStyled() {
    // Consistent Zen popup metrics for every context menu.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f);
    return true;
}

bool ContextMenu::beginItemPopup() {
    beginStyled();
    bool open = ImGui::BeginPopupContextItem(id_);
    if (!open)
        ImGui::PopStyleVar(3);
    return open;
}

bool ContextMenu::beginWindowPopup(bool overItems) {
    beginStyled();
    ImGuiPopupFlags flags = ImGuiPopupFlags_MouseButtonRight;
    if (!overItems)
        flags |= ImGuiPopupFlags_NoOpenOverItems;
    bool open = ImGui::BeginPopupContextWindow(id_, flags);
    if (!open)
        ImGui::PopStyleVar(3);
    return open;
}

bool ContextMenu::beginPopup() {
    beginStyled();
    bool open = ImGui::BeginPopup(id_);
    if (!open)
        ImGui::PopStyleVar(3);
    return open;
}

void ContextMenu::openManually() { ImGui::OpenPopup(id_); }

bool ContextMenu::item(const char* label, const char* shortcut, bool enabled) {
    return ImGui::MenuItem(label, shortcut, false, enabled);
}

bool ContextMenu::checkable(const char* label, bool checked, bool enabled) {
    return ImGui::MenuItem(label, nullptr, checked, enabled);
}

bool ContextMenu::beginSub(const char* label, bool enabled) {
    return ImGui::BeginMenu(label, enabled);
}

void ContextMenu::endSub() { ImGui::EndMenu(); }

void ContextMenu::separator() { ImGui::Separator(); }

void ContextMenu::label(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void ContextMenu::end() {
    ImGui::EndPopup();
    ImGui::PopStyleVar(3);
}

} // namespace crate::ui
