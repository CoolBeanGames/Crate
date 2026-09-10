#include "editor/ScriptEditor.h"

#include "core/Log.h"
#include "script/ClassInfo.h"
#include "script/Format.h"
#include "script/ScriptSystem.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>

namespace crate {

using script::ScriptSystem;

static TextEditor::LanguageDefinition makeCScriptLang() {
    TextEditor::LanguageDefinition d;
    d.mName = "cScript";
    for (const char* k :
         {"class", "func", "var", "return", "if", "else", "switch", "case", "default", "do",
          "do_async", "true", "false", "null", "static", "abstract", "this", "base", "break",
          "continue"})
        d.mKeywords.insert(k);
    for (const char* t : {"int", "float", "bool", "char", "string", "array", "Vector2", "Vector3",
                          "Actor", "Actor2D", "Actor3D", "print", "type_of"}) {
        TextEditor::Identifier id;
        id.mDeclaration = "built-in";
        d.mIdentifiers.insert({t, id});
    }
    using PI = TextEditor::PaletteIndex;
    d.mTokenRegexStrings.push_back({"\\\"(\\\\.|[^\\\"])*\\\"", PI::String});
    d.mTokenRegexStrings.push_back({"\\'\\\\?[^\\']\\'", PI::String});
    d.mTokenRegexStrings.push_back(
        {"[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?[fF]?", PI::Number});
    d.mTokenRegexStrings.push_back({"[a-zA-Z_][a-zA-Z0-9_]*", PI::Identifier});
    d.mTokenRegexStrings.push_back(
        {"[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/\\;\\,\\.]", PI::Punctuation});
    d.mCommentStart = "/*";
    d.mCommentEnd = "*/";
    d.mSingleLineComment = "//";
    d.mCaseSensitive = true;
    d.mAutoIndentation = true;
    return d;
}

ScriptEditor::ScriptEditor() {
    editor_.SetLanguageDefinition(makeCScriptLang());
    editor_.SetPalette(TextEditor::GetDarkPalette());
    editor_.SetTabSize(4);
    editor_.SetShowWhitespaces(false);
}

void ScriptEditor::openScript(const std::string& name) {
    auto* f = ScriptSystem::get().file(name);
    if (!f)
        return;
    current_ = name;
    suppressSync_ = true;
    editor_.SetText(f->source);
    suppressSync_ = false;
    syncGrace_ = 2;
    refreshFunctions();
    acOpen_ = false;
}

void ScriptEditor::refreshFunctions() {
    functions_.clear();
    const auto& types = ScriptSystem::get().types();
    auto it = types.find(current_);
    if (it == types.end() || !it->second->decl)
        return;
    for (const auto& fn : it->second->decl->functions)
        functions_.push_back({fn.name, fn.line});
}

bool ScriptEditor::saveCurrent() {
    auto* f = ScriptSystem::get().file(current_);
    if (!f)
        return false;
    f->source = editor_.GetText();
    return ScriptSystem::get().saveFile(*f);
}

void ScriptEditor::saveAll() {
    for (auto& f : ScriptSystem::get().files())
        if (f.dirty)
            ScriptSystem::get().saveFile(f);
}

bool ScriptEditor::hasUnsaved() const {
    for (const auto& f : ScriptSystem::get().files())
        if (f.dirty)
            return true;
    return false;
}

void ScriptEditor::pushToSystem() {
    if (current_.empty())
        return;
    std::string txt = editor_.GetText();
    ScriptSystem::get().setSource(current_, txt);
    refreshFunctions();
}

void ScriptEditor::draw() {
    if (ScriptSystem::get().files().empty()) {
        ImGui::TextWrapped("No scripts found in assets/scripts/. Create a .cscript file there, "
                           "or use the Asset Browser.");
        return;
    }
    if (current_.empty())
        openScript(ScriptSystem::get().files().front().name);

    // Ctrl+S saves.
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
        saveCurrent();

    ImGui::Columns(2, "script_editor_cols", true);
    static bool sized = false;
    if (!sized) {
        ImGui::SetColumnWidth(0, 220.0f);
        sized = true;
    }
    drawSidebar();
    ImGui::NextColumn();
    drawCode();
    ImGui::Columns(1);
}

void ScriptEditor::drawSidebar() {
    if (ImGui::SmallButton("New")) {
        std::string name = ScriptSystem::get().newScript();
        if (!name.empty())
            openScript(name);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload"))
        ScriptSystem::get().reload();

    ImGui::TextDisabled("SCRIPTS");
    ImGui::BeginChild("scripts", ImVec2(0, 200), true);
    for (auto& f : ScriptSystem::get().files()) {
        std::string label = f.name + (f.dirty ? " *" : "");
        if (!f.error.empty())
            label += "  !";
        if (ImGui::Selectable(label.c_str(), f.name == current_))
            openScript(f.name);
    }
    ImGui::EndChild();

    ImGui::TextDisabled("FUNCTIONS");
    ImGui::BeginChild("functions", ImVec2(0, 0), true);
    for (const auto& [name, line] : functions_) {
        if (ImGui::Selectable(name.c_str()))
            editor_.SetCursorPosition(TextEditor::Coordinates(std::max(0, line - 1), 0));
    }
    ImGui::EndChild();
}

void ScriptEditor::drawCode() {
    // Toolbar
    if (ImGui::Button("Save"))
        saveCurrent();
    ImGui::SameLine();
    if (ImGui::Button("Format")) {
        std::string t = editor_.GetText();
        suppressSync_ = true;
        editor_.SetText(script::reindent(t));
        suppressSync_ = false;
        pushToSystem();
    }
    ImGui::SameLine();
    auto* f = ScriptSystem::get().file(current_);
    if (f && !f->error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(0xC0, 0x32, 0x26).Value);
        ImGui::TextUnformatted(f->error.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Ctrl+S to save   |   Ctrl+Space for completions");
    }

    editor_.SetHandleKeyboardInputs(!acOpen_);
    editor_.Render("##code", ImVec2(0, 0), true);

    if (editor_.IsTextChanged()) {
        if (syncGrace_ > 0)
            --syncGrace_;
        else if (!suppressSync_)
            pushToSystem();
    }

    updateAutocomplete();
}

// --- autocomplete -------------------------------------------------------
void ScriptEditor::updateAutocomplete() {
    ImGuiIO& io = ImGui::GetIO();
    const bool editorFocused = ImGui::IsItemFocused() || acOpen_;

    // current line text up to the caret
    TextEditor::Coordinates cur = editor_.GetCursorPosition();
    std::string line = editor_.GetCurrentLineText();
    int col = std::min<int>(cur.mColumn, (int)line.size());
    std::string upToCaret = line.substr(0, col);

    // word being typed + whether it's a member access (preceded by '.')
    int w = (int)upToCaret.size();
    while (w > 0 && (std::isalnum((unsigned char)upToCaret[w - 1]) || upToCaret[w - 1] == '_'))
        --w;
    std::string word = upToCaret.substr(w);
    bool afterDot = w > 0 && upToCaret[w - 1] == '.';

    // Open on Ctrl+Space or automatically right after typing '.'
    if (editorFocused && !acOpen_) {
        bool ctrlSpace = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space);
        bool justDot = !io.InputQueueCharacters.empty() &&
                       io.InputQueueCharacters.back() == '.';
        if (ctrlSpace || justDot) {
            acOpen_ = true;
            acIndex_ = 0;
        }
    }
    if (!acOpen_)
        return;

    acPrefix_ = word;
    acAfterDot_ = afterDot;

    // Build candidates.
    acItems_.clear();
    auto consider = [&](const std::string& s) {
        if (s.empty())
            return;
        if (acPrefix_.empty() || s.compare(0, acPrefix_.size(), acPrefix_) == 0)
            if (std::find(acItems_.begin(), acItems_.end(), s) == acItems_.end())
                acItems_.push_back(s);
    };

    auto& sys = ScriptSystem::get();
    if (acAfterDot_) {
        // members of every type + Actor built-ins (no full type inference yet)
        for (const auto& d : sys.typeDocs())
            for (const auto& m : d.members)
                consider(m);
    } else {
        for (const auto& c : sys.completions(acPrefix_))
            consider(c);
        // current class's own fields + functions + params
        auto it = sys.types().find(current_);
        if (it != sys.types().end() && it->second->decl) {
            for (const auto& fd : it->second->decl->fields)
                consider(fd.name);
            for (const auto& fn : it->second->decl->functions) {
                consider(fn.name);
                for (const auto& p : fn.params)
                    consider(p.name);
            }
        }
    }
    std::sort(acItems_.begin(), acItems_.end());
    if (acItems_.empty()) {
        acOpen_ = false;
        return;
    }
    acIndex_ = std::clamp(acIndex_, 0, (int)acItems_.size() - 1);

    // keyboard: editor input is disabled while open
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        acIndex_ = (acIndex_ + 1) % (int)acItems_.size();
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        acIndex_ = (acIndex_ + (int)acItems_.size() - 1) % (int)acItems_.size();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        acOpen_ = false;
        return;
    }
    bool accept = ImGui::IsKeyPressed(ImGuiKey_Tab) || ImGui::IsKeyPressed(ImGuiKey_Enter);
    // forward typed characters so the word keeps growing
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        ImWchar c = io.InputQueueCharacters[i];
        if (std::isalnum((int)c) || c == '_') {
            editor_.InsertText(std::string(1, (char)c));
        } else {
            acOpen_ = false; // any other char closes the popup
        }
    }
    io.InputQueueCharacters.resize(0);

    if (accept) {
        const std::string& pick = acItems_[acIndex_];
        if (pick.size() >= acPrefix_.size())
            editor_.InsertText(pick.substr(acPrefix_.size()));
        acOpen_ = false;
        pushToSystem();
    }

    if (!acOpen_)
        return;

    // popup near the caret
    ImVec2 pos = ImGui::GetItemRectMin();
    pos.y = ImGui::GetMousePos().y; // rough; TextEditor lacks a caret-screen-pos API
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x + 60, ImGui::GetItemRectMin().y + 40),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(240, 180), ImGuiCond_Appearing);
    if (ImGui::Begin("##autocomplete", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        for (int i = 0; i < (int)acItems_.size(); ++i) {
            if (ImGui::Selectable(acItems_[i].c_str(), i == acIndex_)) {
                if (acItems_[i].size() >= acPrefix_.size())
                    editor_.InsertText(acItems_[i].substr(acPrefix_.size()));
                acOpen_ = false;
                pushToSystem();
            }
        }
    }
    ImGui::End();
}

} // namespace crate
