#pragma once

namespace crate::ui {

// A small right-click-menu framework used across the editor so every context
// menu looks and behaves the same (Zen styling, consistent spacing, section
// labels, submenus). Thin wrapper over ImGui popups.
//
// Usage:
//   static ui::ContextMenu menu("actor_ctx");
//   ImGui::Selectable("Row");
//   if (menu.beginItemPopup()) {          // opened by right-clicking the row
//       menu.label("EDIT");
//       if (menu.item("Copy", "Ctrl+C")) doCopy();
//       if (menu.item("Paste", "Ctrl+V", canPaste)) doPaste();
//       menu.separator();
//       if (menu.beginSub("Add Child")) {
//           if (menu.item("Mesh")) addMesh();
//           menu.endSub();
//       }
//       bool hidden = actor.hidden();
//       if (menu.checkable("Hidden", hidden)) actor.setHidden(!hidden);
//       menu.end();
//   }
class ContextMenu {
public:
    explicit ContextMenu(const char* strId) : id_(strId) {}

    // Open + begin the popup. Exactly one of these per menu per frame.
    bool beginItemPopup();   // right-click the last-submitted widget
    bool beginWindowPopup(); // right-click empty space in the current window
    bool beginPopup();       // only shows after openManually()
    void openManually();

    // --- items (call only between a successful begin* and end()) -----------
    bool item(const char* label, const char* shortcut = nullptr, bool enabled = true);
    bool checkable(const char* label, bool checked, bool enabled = true);
    bool beginSub(const char* label, bool enabled = true);
    void endSub();
    void separator();
    void label(const char* text); // quiet uppercase section header

    void end();

private:
    bool beginStyled();
    const char* id_;
};

} // namespace crate::ui
