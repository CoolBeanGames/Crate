#pragma once
#include "imgui.h"

#include <functional>
#include <string>
#include <vector>

namespace crate::ui {

// A reusable modal-dialog framework. Configure a Popup once (title, size, body
// callback), call open() to request it, and call draw() every frame while the
// owning panel renders. The body callback builds the contents with the Popup's
// widget helpers; bound variables are edited in place so data flows both ways.
//
//   ui::Popup dlg;
//   dlg.title("Rename").onBody([&](ui::Popup& p){
//       p.inputText("Name", &name);
//   });
//   ...
//   if (ImGui::Button("Rename")) dlg.open();
//   if (dlg.draw() == ui::Popup::Result::Ok) applyRename(name);
class Popup {
public:
    enum class Result { Open, Ok, Cancel };

    Popup& title(std::string t) { title_ = std::move(t); return *this; }
    Popup& size(float w, float h) { size_ = ImVec2(w, h); return *this; }
    Popup& onBody(std::function<void(Popup&)> f) { body_ = std::move(f); return *this; }
    Popup& defaultButtons(bool on) { defaultButtons_ = on; return *this; }
    Popup& okLabel(std::string s) { okLabel_ = std::move(s); return *this; }

    void open() { openRequested_ = true; }
    bool isOpen() const { return isOpen_; }
    Result draw(); // returns Ok / Cancel once on dismissal, otherwise Open

    // --- widgets (call only from the body callback) -----------------------
    void label(const char* text);
    void help(const char* text); // wrapped, dimmed
    void separator() { ImGui::Separator(); }
    void spacing() { ImGui::Spacing(); }
    bool inputText(const char* label, std::string* value, bool focusOnAppear = false);
    bool inputInt(const char* label, int* value);
    bool inputFloat(const char* label, float* value, float step = 0.1f);
    bool checkbox(const char* label, bool* value);
    bool combo(const char* label, int* index, const std::vector<std::string>& options);
    // Scrollable list of strings. Returns true when the selection changes; sets
    // *activated on a double-click (use it to accept()).
    bool listBox(const char* label, int* selected, const std::vector<std::string>& items,
                 bool* activated = nullptr, float height = 180.0f);
    bool button(const char* text);

    // Dismiss the popup from inside the body.
    void accept() { pending_ = Result::Ok; }
    void cancel() { pending_ = Result::Cancel; }

private:
    std::string title_ = "Popup";
    std::string okLabel_ = "OK";
    ImVec2 size_{360, 0};
    std::function<void(Popup&)> body_;
    bool defaultButtons_ = true;
    bool openRequested_ = false;
    bool isOpen_ = false;
    Result pending_ = Result::Open;
};

} // namespace crate::ui
