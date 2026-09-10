#pragma once
#include "editor/Popup.h"

#include <string>
#include <vector>

namespace crate {

class MaterialLibrary;
class Actor;

// What kind of reference a picker field edits.
enum class PickKind { Material, Mesh, Texture, Actor };

// Drag-drop payload id for a given kind (asset-browser rows use these).
const char* pickPayloadId(PickKind kind);

// Data sources the picker draws its candidate list from.
struct PickerSources {
    const MaterialLibrary* materials = nullptr;
    const std::vector<std::string>* importedAssets = nullptr; // model keys + image paths
    Actor* sceneRoot = nullptr;
};

// A reusable "reference field": renders the current value with a browse button
// that opens a modal list (matching assets + matching scene actors, plus
// <none>), double-click or OK to choose. Built on ui::Popup. One instance can
// back many fields. Returns true the frame `value` changes.
class AssetPicker {
public:
    bool field(const char* label, PickKind kind, std::string* value, const PickerSources& src);

private:
    void rebuild(PickKind kind, const PickerSources& src);

    ui::Popup popup_;
    std::string openField_;
    std::vector<std::string> candidates_; // display == value written ("" = clear)
    int selected_ = -1;
};

} // namespace crate
