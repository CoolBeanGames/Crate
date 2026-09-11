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
    void applyAutoBrackets();   // auto-insert/skip-over matching } and )
    // Pre-insert bracket pairing/skip-over used while the autocomplete popup
    // is open (TextEditor isn't handling keyboard input in that case, so
    // characters are forwarded manually -- see updateAutocomplete()).
    // Returns true if `c` was fully handled (nothing else should insert it).
    bool handleBracketCharForAc(char c);
    // Best-effort static type of a dotted member-access chain (e.g.
    // "transform.rotation" -> "Vector3"), used to narrow '.' completions to
    // the members that actually exist on the receiver. Empty => unknown.
    std::string resolveChainType(const std::string& chain) const;
    // Local variable names declared (at any depth) in the function whose
    // body contains `line` (0-based), for live autocomplete of locals.
    void collectLocalsInScope(int line, std::vector<std::string>& out) const;

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

    // Per-frame input snapshot, captured *before* editor_.Render() (which
    // drains ImGui's character queue and consumes the keypress), so the
    // bracket/indent/autocomplete logic that runs after Render() can still
    // see what was actually typed this frame.
    std::vector<ImWchar> typedChars_;
    TextEditor::Coordinates preCursor_{0, 0};
    std::string preLine_;
    bool enterPressed_ = false;
};

} // namespace crate
