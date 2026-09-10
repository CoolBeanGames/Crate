#pragma once
#include "editor/Popup.h"
#include "imgui_texteditor/TextEditor.h"

#include <string>
#include <utility>
#include <vector>

namespace crate {

// The in-engine script IDE: script list + function list on the left, a
// syntax-highlighted code editor with autocomplete on the right. Backed by
// script::ScriptSystem for sources, types and completions.
class ScriptEditor {
public:
    ScriptEditor();

    // Full-area editor UI (called instead of the normal panels while in
    // "Script Editor" mode).
    void draw();

    void openScript(const std::string& name);
    bool saveCurrent();
    void saveAll();
    bool hasUnsaved() const;

    // Start the "name your new script" prompt (the class gets the same name).
    void beginNewScript();

    const std::string& current() const { return current_; }

private:
    void drawSidebar();
    void drawCode();
    void pushToSystem();     // editor text -> ScriptSystem (recompile)
    void refreshFunctions();
    void updateAutocomplete();
    void applyElectricIndent(); // +1 indent after '{' on Enter, snap '}' back

    TextEditor editor_;
    std::string current_;
    std::vector<std::pair<std::string, int>> functions_; // name, line
    bool suppressSync_ = false;
    int syncGrace_ = 0; // skip the spurious text-changed right after SetText

    ui::Popup newPopup_;
    std::string newName_ = "NewScript";
    bool newRequested_ = false;

    // autocomplete state
    bool acOpen_ = false;
    bool acAfterDot_ = false;
    std::string acPrefix_;
    std::vector<std::string> acItems_;
    int acIndex_ = 0;
};

} // namespace crate
