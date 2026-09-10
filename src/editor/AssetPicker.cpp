#include "editor/AssetPicker.h"

#include "assets/Image.h"
#include "assets/MaterialLibrary.h"
#include "scene/Actor.h"
#include "scene/Actor2D.h"
#include "scene/BuiltinComponents.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>

namespace crate {

static bool isImagePath(const std::string& p) {
    auto dot = p.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    std::string e = p.substr(dot + 1);
    for (char& c : e)
        c = (char)std::tolower((unsigned char)c);
    return isSupportedImageExt(e);
}

static void walkActors(Actor& node, const std::function<void(Actor&)>& fn) {
    fn(node);
    for (const auto& c : node.children())
        walkActors(*c, fn);
}

const char* pickPayloadId(PickKind kind) {
    switch (kind) {
        case PickKind::Material: return "AP_MATERIAL";
        case PickKind::Mesh: return "AP_MESH";
        case PickKind::Texture: return "AP_TEXTURE";
        case PickKind::Actor: return "AP_ACTOR";
    }
    return "AP_?";
}

static Actor* findById(Actor* root, uint64_t id) {
    Actor* hit = nullptr;
    if (root)
        walkActors(*root, [&](Actor& a) {
            if (a.id() == id)
                hit = &a;
        });
    return hit;
}

// Pull the reference of `kind` out of a dragged actor's components.
static std::string refFromActor(PickKind kind, Actor& a) {
    if (kind == PickKind::Actor)
        return a.name();
    if (auto* mr = a.getComponent<MeshRenderer>()) {
        if (kind == PickKind::Mesh)
            return mr->usePrimitive ? mr->primitive : mr->meshPath;
        if (kind == PickKind::Texture)
            return mr->texturePath;
        if (kind == PickKind::Material)
            return mr->materialRef;
    }
    if (kind == PickKind::Texture)
        if (auto* sp = dynamic_cast<SpriteActor*>(&a))
            return sp->texturePath;
    return {};
}

void AssetPicker::rebuild(PickKind kind, const PickerSources& src) {
    candidates_.clear();
    candidates_.push_back(""); // <none> / clear

    switch (kind) {
        case PickKind::Material:
            if (src.materials)
                for (auto& n : src.materials->names())
                    candidates_.push_back(n);
            break;
        case PickKind::Mesh:
            for (const char* p : {"Cube", "Sphere", "Cylinder", "Capsule", "Plane", "Quad"})
                candidates_.push_back(p);
            if (src.importedAssets)
                for (const auto& a : *src.importedAssets)
                    if (!isImagePath(a))
                        candidates_.push_back(a);
            // scene actors that carry a mesh (pick copies their meshPath)
            if (src.sceneRoot)
                walkActors(*src.sceneRoot, [&](Actor& a) {
                    if (auto* mr = a.getComponent<MeshRenderer>())
                        candidates_.push_back(mr->usePrimitive ? mr->primitive : mr->meshPath);
                });
            break;
        case PickKind::Texture:
            if (src.importedAssets)
                for (const auto& a : *src.importedAssets)
                    if (isImagePath(a))
                        candidates_.push_back(a);
            break;
        case PickKind::Actor:
            if (src.sceneRoot)
                walkActors(*src.sceneRoot, [&](Actor& a) {
                    if (&a != src.sceneRoot)
                        candidates_.push_back(a.name());
                });
            break;
    }

    // de-dup while keeping order
    std::vector<std::string> uniq;
    for (auto& c : candidates_)
        if (std::find(uniq.begin(), uniq.end(), c) == uniq.end())
            uniq.push_back(c);
    candidates_ = std::move(uniq);
}

bool AssetPicker::field(const char* label, PickKind kind, std::string* value,
                        const PickerSources& src) {
    ImGui::PushID(label);
    bool changed = false;

    ImGui::BeginGroup();
    ImGui::TextUnformatted(value->empty() ? "<none>" : value->c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("...")) {
        openField_ = label;
        rebuild(kind, src);
        selected_ = 0;
        for (int i = 0; i < (int)candidates_.size(); ++i)
            if (candidates_[i] == *value)
                selected_ = i;
        popup_.title(std::string("Pick ") + label)
            .size(360, 380)
            .onBody([this](ui::Popup& p) {
                std::vector<std::string> disp;
                disp.reserve(candidates_.size());
                for (auto& c : candidates_)
                    disp.push_back(c.empty() ? "<none>" : c);
                bool activated = false;
                p.listBox("##list", &selected_, disp, &activated);
                if (activated)
                    p.accept();
            })
            .open();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", label);
    ImGui::EndGroup();

    // Drag an asset (from the browser) or an actor (from the hierarchy) onto
    // the field to set it, if the type matches.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(pickPayloadId(kind))) {
            std::string s(static_cast<const char*>(p->Data));
            if (*value != s) {
                *value = s;
                changed = true;
            }
        } else if (const ImGuiPayload* pa = ImGui::AcceptDragDropPayload("CRATE_ACTOR")) {
            uint64_t id = *static_cast<const uint64_t*>(pa->Data);
            if (Actor* dragged = findById(src.sceneRoot, id)) {
                std::string s = refFromActor(kind, *dragged);
                if (!s.empty() && *value != s) {
                    *value = s;
                    changed = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (openField_ == label) {
        if (popup_.draw() == ui::Popup::Result::Ok && selected_ >= 0 &&
            selected_ < (int)candidates_.size()) {
            if (*value != candidates_[selected_]) {
                *value = candidates_[selected_];
                changed = true;
            }
            openField_.clear();
        }
    }

    ImGui::PopID();
    return changed;
}

} // namespace crate
