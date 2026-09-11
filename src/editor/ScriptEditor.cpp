#include "editor/ScriptEditor.h"

#include "core/Log.h"
#include "script/ClassInfo.h"
#include "script/Format.h"
#include "script/ScriptSystem.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <climits>

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
    std::string nowName = ScriptSystem::get().setSource(current_, txt);
    if (!nowName.empty())
        current_ = nowName; // follow a class rename
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

void ScriptEditor::beginNewScript() {
    newName_ = "NewScript";
    newRequested_ = true;
}

void ScriptEditor::drawSidebar() {
    if (ImGui::SmallButton("New"))
        beginNewScript();
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload"))
        ScriptSystem::get().reload();

    if (newRequested_) {
        newRequested_ = false;
        newPopup_.title("New Script")
            .size(320, 0)
            .onBody([this](ui::Popup& p) {
                p.help("The class gets this name too. Must start with an uppercase letter.");
                p.inputText("Name", &newName_, /*focusOnAppear=*/true);
            })
            .open();
    }
    if (newPopup_.draw() == ui::Popup::Result::Ok && !newName_.empty()) {
        std::string made = ScriptSystem::get().newScript(newName_);
        if (!made.empty())
            openScript(made);
    }

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

    // Snapshot input *before* Render(): TextEditor's own keyboard handling
    // (which runs inside Render() when acOpen_ is false) drains
    // io.InputQueueCharacters and consumes the Enter keypress, so anything
    // downstream that wants to know what was just typed has to look now.
    ImGuiIO& io = ImGui::GetIO();
    typedChars_.assign(io.InputQueueCharacters.begin(), io.InputQueueCharacters.end());
    preCursor_ = editor_.GetCursorPosition();
    preLine_ = editor_.GetCurrentLineText();
    enterPressed_ = ImGui::IsKeyPressed(ImGuiKey_Enter, false);

    editor_.SetHandleKeyboardInputs(!acOpen_);
    editor_.Render("##code", ImVec2(0, 0), true);
    applyAutoBrackets();
    applyElectricIndent();

    if (editor_.IsTextChanged()) {
        if (syncGrace_ > 0)
            --syncGrace_;
        else if (!suppressSync_)
            pushToSystem();
    }

    updateAutocomplete();
}

// --- auto indent -------------------------------------------------------
static int leadingTabs(const std::string& s) {
    int n = 0;
    for (char c : s) {
        if (c == '\t')
            ++n;
        else
            break;
    }
    return n;
}

static char lastCodeChar(const std::string& line) {
    std::string code = line;
    if (auto c = code.find("//"); c != std::string::npos)
        code = code.substr(0, c);
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t' || code.back() == '\r'))
        code.pop_back();
    return code.empty() ? '\0' : code.back();
}

// --- auto brackets ------------------------------------------------------
// Typing '{' or '(' inserts the matching closer and leaves the caret between
// them; typing a closer that's already sitting right at the caret (i.e. the
// one we just auto-inserted) steps over it instead of adding a duplicate.
void ScriptEditor::applyAutoBrackets() {
    if (acOpen_ || !ImGui::IsItemFocused())
        return;
    // Only handle the common case of a single typed character: bulk input
    // (e.g. a paste that went through the character queue) is left alone.
    if (typedChars_.size() != 1)
        return;
    char c = (char)typedChars_[0];

    if (c == '{' || c == '(') {
        // Render() already inserted `c` and moved the caret past it; add the
        // matching closer right after and step back in between.
        editor_.InsertText(c == '{' ? "}" : ")");
        auto cur = editor_.GetCursorPosition();
        editor_.SetCursorPosition(TextEditor::Coordinates(cur.mLine, cur.mColumn - 1));
        return;
    }
    if (c == '}' || c == ')') {
        // Render() already inserted a second `c` right before whatever was
        // at the caret. If that was the *same* closer, it's a type-over:
        // undo the duplicate and just step past the existing one.
        if (preCursor_.mColumn >= 0 && preCursor_.mColumn < (int)preLine_.size() &&
            preLine_[preCursor_.mColumn] == c) {
            // Backspace() is private; step back onto the just-typed
            // duplicate and forward-delete it instead.
            auto cur = editor_.GetCursorPosition();
            editor_.SetCursorPosition(TextEditor::Coordinates(cur.mLine, cur.mColumn - 1));
            editor_.Delete();
            cur = editor_.GetCursorPosition();
            editor_.SetCursorPosition(TextEditor::Coordinates(cur.mLine, cur.mColumn + 1));
        }
    }
}

// Used by updateAutocomplete() while the popup is open: at that point
// TextEditor isn't handling keyboard input, so characters must be forwarded
// (or paired/skipped) manually, before anything is inserted. Returns true if
// `c` was fully handled.
bool ScriptEditor::handleBracketCharForAc(char c) {
    if (c == '{' || c == '(') {
        editor_.InsertText(std::string(1, c) + (c == '{' ? "}" : ")"));
        auto cur = editor_.GetCursorPosition();
        editor_.SetCursorPosition(TextEditor::Coordinates(cur.mLine, cur.mColumn - 1));
        return true;
    }
    if (c == '}' || c == ')') {
        std::string line = editor_.GetCurrentLineText();
        auto cur = editor_.GetCursorPosition();
        if (cur.mColumn >= 0 && cur.mColumn < (int)line.size() && line[cur.mColumn] == c) {
            editor_.SetCursorPosition(TextEditor::Coordinates(cur.mLine, cur.mColumn + 1));
            return true;
        }
    }
    return false;
}

void ScriptEditor::applyElectricIndent() {
    if (!ImGui::IsItemFocused() || acOpen_)
        return;

    auto cur = editor_.GetCursorPosition();
    std::vector<std::string> lines = editor_.GetTextLines();
    if (cur.mLine < 0 || cur.mLine >= (int)lines.size())
        return;

    // Enter: TextEditor already copied the previous line's indent; if that line
    // opened a brace, go one level deeper. If Enter was pressed right between
    // an auto-inserted matching pair on the same original line (e.g. "{}" or
    // "()"), split it into three lines instead: the closer drops to its own
    // line back at the opening indent, and the caret lands on a fresh,
    // one-level-deeper body line in between.
    if (enterPressed_ && cur.mLine > 0) {
        const std::string& prevLine = lines[cur.mLine - 1];
        const std::string& newLine = lines[cur.mLine];
        char prevLast = lastCodeChar(prevLine);

        size_t nf = newLine.find_first_not_of("\t ");
        char newFirst = nf == std::string::npos ? '\0' : newLine[nf];
        bool onlyCloser = nf != std::string::npos &&
                          newLine.find_first_not_of("\t ", nf + 1) == std::string::npos &&
                          (newFirst == '}' || newFirst == ')');
        bool matchingPair = onlyCloser && ((prevLast == '{' && newFirst == '}') ||
                                           (prevLast == '(' && newFirst == ')'));
        if (matchingPair) {
            int openIndent = leadingTabs(prevLine);
            editor_.InsertText("\t\n" + std::string(openIndent, '\t'));
            editor_.SetCursorPosition(TextEditor::Coordinates(cur.mLine, openIndent + 1));
            return;
        }
        if (prevLast == '{')
            editor_.InsertText("\t");
        return;
    }

    // A line that is only a closing brace: snap it to (brace depth - 1) tabs.
    const std::string& line = lines[cur.mLine];
    size_t first = line.find_first_not_of("\t ");
    if (first == std::string::npos || line[first] != '}')
        return;
    if (line.find_first_not_of("\t ", first + 1) != std::string::npos)
        return; // something after the '}'

    int depth = 0;
    for (int i = 0; i < cur.mLine; ++i)
        for (char c : lines[i]) {
            if (c == '{')
                ++depth;
            else if (c == '}')
                --depth;
        }
    int desired = depth > 0 ? depth - 1 : 0;
    int have = leadingTabs(line);
    if (have > desired) {
        int remove = have - desired;
        editor_.SetCursorPosition(TextEditor::Coordinates(cur.mLine, 0));
        for (int k = 0; k < remove; ++k)
            editor_.Delete();
        editor_.SetCursorPosition(TextEditor::Coordinates(cur.mLine, std::max(0, cur.mColumn - remove)));
    }
}

// --- autocomplete -------------------------------------------------------

// Best-effort static type for a builtin member (Actor/Transform/Vector3/...).
// Returns "" when the member's type isn't a builtin we know about (either
// because it doesn't exist, or because it belongs to a script class -- those
// are resolved separately via ClassInfo).
static std::string builtinMemberType(const std::string& type, const std::string& member) {
    if (type == "Actor" || type == "Actor2D" || type == "Actor3D" || type == "Transform") {
        if (member == "position" || member == "rotation" || member == "scale" ||
            member == "forward" || member == "right" || member == "up")
            return "Vector3";
        if (member == "name")
            return "string";
    }
    if (type == "Vector3" || type == "Vector2") {
        if (member == "x" || member == "y" || member == "z")
            return "float";
        if (member == "str")
            return "string";
    }
    if ((type == "array" || type == "string") && member == "length")
        return "int";
    return "";
}

std::string ScriptEditor::resolveChainType(const std::string& chain) const {
    if (chain.empty())
        return "";
    std::vector<std::string> segs;
    size_t start = 0;
    while (start <= chain.size()) {
        size_t dot = chain.find('.', start);
        segs.push_back(chain.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    if (segs.empty() || segs[0].empty())
        return "";

    auto& sys = ScriptSystem::get();
    std::string type;
    if (segs[0] == "transform" || segs[0] == "actor") {
        type = "Actor";
    } else {
        auto it = sys.types().find(current_);
        if (it != sys.types().end() && it->second->decl) {
            for (const auto& fd : it->second->decl->fields)
                if (fd.name == segs[0] && !fd.type.empty())
                    type = fd.type;
            if (type.empty())
                for (const auto& fn : it->second->decl->functions)
                    for (const auto& p : fn.params)
                        if (p.name == segs[0] && !p.type.empty())
                            type = p.type;
        }
    }
    if (type.empty())
        return "";

    for (size_t i = 1; i < segs.size(); ++i) {
        const std::string& member = segs[i];
        std::string next = builtinMemberType(type, member);
        if (next.empty()) {
            // Maybe `type` is a script class: look up the field/function.
            auto it = sys.types().find(type);
            if (it != sys.types().end() && it->second->decl) {
                for (const auto& fd : it->second->decl->fields)
                    if (fd.name == member)
                        next = fd.type;
                if (next.empty())
                    for (const auto& fn : it->second->decl->functions)
                        if (fn.name == member)
                            next = fn.returnType;
            }
        }
        if (next.empty())
            return ""; // unknown from here on; caller falls back
        type = next;
    }
    return type;
}

static void collectLocalsInStmts(const std::vector<script::StmtPtr>& stmts, std::vector<std::string>& out);

static void collectLocalsInStmt(const script::Stmt& s, std::vector<std::string>& out) {
    using script::StmtKind;
    if (s.kind == StmtKind::VarDecl && !s.name.empty())
        out.push_back(s.name);
    collectLocalsInStmts(s.thenBody, out);
    collectLocalsInStmts(s.elseBody, out);
    collectLocalsInStmts(s.body, out);
    for (const auto& c : s.cases)
        collectLocalsInStmts(c.body, out);
}

static void collectLocalsInStmts(const std::vector<script::StmtPtr>& stmts, std::vector<std::string>& out) {
    for (const auto& s : stmts)
        if (s)
            collectLocalsInStmt(*s, out);
}

void ScriptEditor::collectLocalsInScope(int line, std::vector<std::string>& out) const {
    auto& sys = ScriptSystem::get();
    auto it = sys.types().find(current_);
    if (it == sys.types().end() || !it->second->decl)
        return;
    const auto& fns = it->second->decl->functions;
    for (size_t i = 0; i < fns.size(); ++i) {
        int start = fns[i].line;
        int end = i + 1 < fns.size() ? fns[i + 1].line : INT_MAX;
        if (line + 1 < start || line + 1 >= end)
            continue; // not the enclosing function (functions' `line` is 1-based)
        for (const auto& p : fns[i].params)
            out.push_back(p.name);
        collectLocalsInStmts(fns[i].body, out);
        break;
    }
}

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

    // The dotted receiver chain right before the word, e.g. for
    // "transform.rotation.y" -> "transform.rotation" (word == "y").
    std::string chain;
    if (afterDot) {
        int p = w - 1; // sits on the '.'
        while (true) {
            int idEnd = p;
            int idStart = idEnd;
            while (idStart > 0 &&
                   (std::isalnum((unsigned char)upToCaret[idStart - 1]) || upToCaret[idStart - 1] == '_'))
                --idStart;
            if (idStart == idEnd)
                break;
            p = idStart;
            if (p > 0 && upToCaret[p - 1] == '.') {
                p -= 1;
                continue;
            }
            break;
        }
        chain = upToCaret.substr(p, (w - 1) - p);
    }

    // Open on Ctrl+Space, right after typing '.', or as soon as an
    // identifier character is typed -- autocomplete should be live, not
    // something you have to remember to summon.
    if (editorFocused && !acOpen_) {
        bool ctrlSpace = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space);
        bool justTypedWordChar = false;
        for (ImWchar tc : typedChars_)
            if (tc == '.' || std::isalnum((int)tc) || tc == '_')
                justTypedWordChar = true;
        if (ctrlSpace || (justTypedWordChar && (afterDot || !word.empty()))) {
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
        std::string type = resolveChainType(chain);
        const ScriptSystem::TypeDoc* doc = type.empty() ? nullptr : sys.typeDoc(type);
        if (doc) {
            // Known receiver type: only its own members (+ inherited, walking
            // script base classes) -- no more dumping every type's members.
            for (const ScriptSystem::TypeDoc* d = doc; d;
                 d = d->base.empty() ? nullptr : sys.typeDoc(d->base))
                for (const auto& m : d->members)
                    consider(m);
        } else {
            // Unknown receiver: fall back to every known member (better a
            // noisy list than none at all).
            for (const auto& d : sys.typeDocs())
                for (const auto& m : d.members)
                    consider(m);
        }
    } else {
        for (const auto& c : sys.completions(acPrefix_))
            consider(c);
        // current class's own fields + functions + params, plus locals
        // declared anywhere in the function the caret is currently inside.
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
        std::vector<std::string> locals;
        collectLocalsInScope(cur.mLine, locals);
        for (const auto& n : locals)
            consider(n);
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
    // forward typed characters so the word keeps growing (TextEditor isn't
    // handling keyboard input while the popup is open, so nothing else will
    // insert these) -- brackets get paired/skipped just like normal typing,
    // and anything else still reaches the buffer instead of being dropped.
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        ImWchar c = io.InputQueueCharacters[i];
        if (std::isalnum((int)c) || c == '_') {
            editor_.InsertText(std::string(1, (char)c));
        } else if (c == '.') {
            // Chained access (e.g. "transform." then "rotation."): insert the
            // dot and keep the popup open, now completing the new receiver's
            // members, instead of closing and losing the live trigger.
            editor_.InsertText(".");
            acIndex_ = 0;
        } else if (handleBracketCharForAc((char)c)) {
            acOpen_ = false;
        } else {
            if (c != 0)
                editor_.InsertText(std::string(1, (char)c));
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
