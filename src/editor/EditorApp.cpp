#include "editor/EditorApp.h"
#include "assets/AssetDatabase.h"
#include "assets/FbxImport.h"
#include "assets/Image.h"
#include "input/Input.h"
#include "editor/Console.h"
#include "platform/FileDialog.h"
#include "editor/ContextMenu.h"
#include "editor/Theme.h"
#include "core/Log.h"
#include "scene/Actor2D.h"
#include "scene/Actor3D.h"
#include "scene/BuiltinComponents.h"
#include "scene/ComponentRegistry.h"
#include "editor/ScriptBuild.h"
#include "script/NativeClassRegistry.h"
#include "script/ScriptSystem.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ImGuizmo.h"

#include <filesystem>

namespace crate {
namespace fs = std::filesystem;

static const char* kActorPayload = "CRATE_ACTOR";

// Resolve an actor by id anywhere in the tree.
static Actor* findById(Actor& node, uint64_t id) {
    if (node.id() == id)
        return &node;
    for (const auto& c : node.children())
        if (Actor* hit = findById(*c, id))
            return hit;
    return nullptr;
}

// First CameraComponent found (depth-first), optionally skipping `exclude`
// and/or requiring `enabled`. Backs both Game View's active-camera lookup and
// the exclusive-activation bookkeeping below (task 77).
static CameraComponent* findCamera(Actor& node, bool requireEnabled, CameraComponent* exclude) {
    if (auto* cc = node.getComponent<CameraComponent>())
        if (cc != exclude && (!requireEnabled || cc->enabled))
            return cc;
    for (const auto& c : node.children())
        if (CameraComponent* hit = findCamera(*c, requireEnabled, exclude))
            return hit;
    return nullptr;
}

static void disableOtherCameras(Actor& node, CameraComponent* keep) {
    if (auto* cc = node.getComponent<CameraComponent>())
        if (cc != keep)
            cc->enabled = false;
    for (const auto& c : node.children())
        disableOtherCameras(*c, keep);
}

EditorApp::EditorApp() : scene_(Scene::makeSample()) {
    registerBuiltinComponents();
    AssetDatabase::get().load(assetDir_);
    script::ScriptSystem::get().loadFolder(assetDir_ + "/scripts");
    scanAssets();
    loadFolderColors();

    // Load the first input map found under assets/ as the active one.
    {
        std::error_code ec;
        if (fs::exists(assetDir_, ec))
            for (const auto& e : fs::recursive_directory_iterator(assetDir_, ec))
                if (e.is_regular_file(ec) && e.path().extension() == ".inputmap") {
                    inputMapPath_ = e.path().generic_string();
                    inputMap_.load(inputMapPath_);
                    Input::get().setMap(&inputMap_);
                    break;
                }
    }
    renderer_.setMeshLibrary(&meshLib_);
    renderer_.setMaterialLibrary(&materialLib_);
    renderer_.loadLightmap(scene_, assetDir_); // apply any lightmap baked for this scene
    CR_LOG("app", "Crate editor started");
    CR_LOG("scene", "Loaded sample scene with " + std::to_string(scene_.actorCount()) + " actors");
}

EditorApp::~EditorApp() = default;

static std::string lowerExt(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

void EditorApp::ingestDroppedFile(const std::string& path) {
    const std::string ext = lowerExt(path);
    if (ext == "fbx") {
        renderer_.invalidateMesh(path);
        FbxImportResult res = importFbx(path, meshLib_, materialLib_);
        if (!res.ok) {
            CR_ERROR("assets", "Import failed: " + (res.error.empty() ? path : res.error));
            return;
        }
        if (std::find(importedAssets_.begin(), importedAssets_.end(), res.key) ==
            importedAssets_.end())
            importedAssets_.push_back(res.key);
        CR_LOG("assets", "Asset " + res.key + " -> " + AssetDatabase::get().idFor(res.key));

        std::string name = path;
        if (auto slash = name.find_last_of("/\\"); slash != std::string::npos)
            name = name.substr(slash + 1);
        auto actor = std::make_unique<Actor3D>(name);
        auto mr = std::make_unique<MeshRenderer>();
        mr->usePrimitive = false;
        mr->meshPath = res.key;
        if (!res.materialNames.empty())
            mr->materialRef = res.materialNames.front();
        actor->addComponent(std::move(mr));
        Actor* added = scene_.add(std::move(actor));
        scene_.select(added);
        CR_LOG("scene", "Added imported model '" + name + "' to the scene");
    } else if (isSupportedImageExt(ext)) {
        Image probe = loadImage(path);
        if (!probe.valid()) {
            CR_ERROR("assets", "Could not decode image: " + path);
            return;
        }
        if (std::find(importedAssets_.begin(), importedAssets_.end(), path) == importedAssets_.end())
            importedAssets_.push_back(path);
        AssetDatabase::get().idFor(path);
        CR_LOG("assets", "Imported image " + path + " (" + std::to_string(probe.width) + "x" +
                             std::to_string(probe.height) + ")");
        Actor* sel = scene_.selected();
        if (auto* mr = sel ? sel->getComponent<MeshRenderer>() : nullptr) {
            mr->texturePath = path;
            renderer_.invalidateTexture(path);
            CR_LOG("assets", "Applied texture to '" + sel->name() + "'");
        }
    } else if (ext == "cscript") {
        // Copy the script into the project's scripts folder and load it.
        auto& sys = script::ScriptSystem::get();
        std::string dir = sys.scriptsDir().empty() ? (assetDir_ + "/scripts") : sys.scriptsDir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path dst = fs::path(dir) / fs::path(path).filename();
        if (fs::path(path) != dst)
            fs::copy_file(path, dst, fs::copy_options::overwrite_existing, ec);
        sys.loadFolder(dir);
        AssetDatabase::get().idFor(dst.generic_string());
        CR_LOG("assets", "Imported script " + dst.filename().string());
    } else {
        CR_WARN("assets", "Unsupported drop: " + path);
    }
}

void EditorApp::attachDevice(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) {
        renderer_.shutdown();
        return;
    }
    if (renderer_.init(device, context))
        CR_LOG("render", "Viewport renderer attached");
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void EditorApp::onFrame() {
    if (firstFrame_) {
        CR_LOG("render", "First frame presented");
        firstFrame_ = false;
    }
    if (!gizmoInit_) {
        ImGuizmo::SetImGuiContext(ImGui::GetCurrentContext());
        gizmoInit_ = true;
    }
    ImGuizmo::BeginFrame();
    const float dt = ImGui::GetIO().DeltaTime;
    if (scene_.selected()) {
        // An actor selection supersedes an asset selection (kept exclusive so
        // Delete is unambiguous).
        selectedMaterial_.clear();
        selectedAsset_.clear();
        selectedScript_.clear();
    }

    // Play mode: tick components (frame update + fixed-step physics).
    if (playing_) {
        Input::get().poll();
        script::ScriptSystem::get().dispatchInput();
        scene_.tick(dt);
        script::ScriptSystem::get().tickStatics(dt);
        physicsAccum_ += dt;
        const float step = 1.0f / 60.0f;
        int guard = 0;
        while (physicsAccum_ >= step && guard++ < 8) {
            scene_.physicsTick(step);
            script::ScriptSystem::get().physicsStatics(step);
            physicsAccum_ -= step;
        }
        // Actor.destroy() / Component.remove(): actually applied here, once
        // every script this frame has finished running, never mid-hook.
        script::ScriptSystem::get().flushPending(scene_);
    }

    // Global editor shortcuts (skipped while typing in a field).
    if (!ImGui::GetIO().WantTextInput) {
        if (Actor* s = scene_.selected()) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copyActor(s);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) cutActor(s);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) scene_.select(scene_.duplicate(s));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))
            deleteSelection();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V))
            scene_.select(pasteInto(scene_.selected()));
        // Gizmo mode: W / E / R (Blender/Unity-ish).
        if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmoOp_ = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) gizmoOp_ = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) gizmoOp_ = 2;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##CrateDockHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    drawMenuBar();
    drawToolbar();

    ImGuiID dockspace_id = ImGui::GetID("CrateDockspace");
    if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetContentRegionAvail());

        ImGuiID center = dockspace_id;
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);

        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderDockWindow("Asset Browser", bottom);
        ImGui::DockBuilderDockWindow("Engine Console", bottom);
        ImGui::DockBuilderDockWindow("Game Console", bottom);
        ImGui::DockBuilderFinish(dockspace_id);
    }
    ImGui::DockSpace(dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_None);

    ImGui::End(); // dock host

    drawHierarchy();
    drawInspector();
    drawViewport();
    drawBottomPanel();
    if (inputMapOpen_)
        drawInputMapEditor();

    if (showDemo_)
        ImGui::ShowDemoWindow(&showDemo_);
}

// ---------------------------------------------------------------------------
// Top: menu bar + play/stop toolbar
// ---------------------------------------------------------------------------
void EditorApp::drawMenuBar() {
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene")) {
            scene_ = Scene("Untitled");
            renderer_.loadLightmap(scene_, assetDir_);
            CR_LOG("scene", "New scene created");
        }
        if (ImGui::MenuItem("Load Sample Scene")) {
            scene_ = Scene::makeSample();
            renderer_.loadLightmap(scene_, assetDir_);
            CR_LOG("scene", "Reloaded sample scene");
        }
        if (ImGui::MenuItem("Open Scene..."))
            openScene();
        ImGui::Separator();
        if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
            saveScene();
        if (ImGui::MenuItem("Save Scene As..."))
            saveSceneAs();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        Actor* s = scene_.selected();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, s)) cutActor(s);
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, s)) copyActor(s);
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, clipboard_ != nullptr))
            scene_.select(pasteInto(s));
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, s)) scene_.select(scene_.duplicate(s));
        bool anySel = s || !selectedMaterial_.empty() || !selectedAsset_.empty();
        if (ImGui::MenuItem("Delete", "Del", false, anySel)) deleteSelection();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("ImGui Demo", nullptr, &showDemo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Object")) {
        Actor* p = scene_.selected();
        if (ImGui::MenuItem("Create Empty Actor")) scene_.select(spawn("actor", p));
        if (ImGui::MenuItem("Create 3D Actor"))    scene_.select(spawn("actor3d", p));
        if (ImGui::MenuItem("Create Mesh"))        scene_.select(spawn("mesh", p));
        if (ImGui::MenuItem("Create Sprite"))      scene_.select(spawn("sprite", p));
        if (ImGui::MenuItem("Create UI Control"))  scene_.select(spawn("ui", p));
        if (ImGui::BeginMenu("Rendering")) {
            if (ImGui::MenuItem("Camera"))            scene_.select(spawn("camera", p));
            if (ImGui::MenuItem("Directional Light")) scene_.select(spawn("light_directional", p));
            if (ImGui::MenuItem("Point Light"))       scene_.select(spawn("light_point", p));
            if (ImGui::MenuItem("Spot Light"))        scene_.select(spawn("light_spot", p));
            if (ImGui::MenuItem("Fog"))               scene_.select(spawn("fog", p));
            if (ImGui::MenuItem("Volumetric Fog"))    scene_.select(spawn("volumetricfog", p));
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Bake Lightmaps")) {
            renderer_.bakeLighting(scene_, assetDir_);
            CR_LOG("render", "Baked static lights into meshes and light probes");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        ImGui::TextDisabled("Panels are docked; drag tabs to rearrange.");
        ImGui::EndMenu();
    }

    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(ImGui::GetCursorPosX() + w - 90.0f);
    if (ImGui::BeginMenu("Settings")) {
        ImGui::Text("Frame: %.1f FPS", ImGui::GetIO().Framerate);
        ImGui::Checkbox("ImGui Demo", &showDemo_);
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void EditorApp::drawToolbar() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 6));
    ImGui::Dummy(ImVec2(1, 2));

    // Snapshot before the button: its click handler flips playing_
    // synchronously, so gating the push/pop on the live (mutable-mid-frame)
    // flag instead of this snapshot produces an unbalanced PopStyleColor on
    // every click (Debug builds assert/abort; Release corrupts the style
    // stack silently).
    const bool wasPlaying = playing_;
    const char* playLabel = wasPlaying ? "  Pause  " : "  Play  ";
    if (wasPlaying)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::SameLine(0, 12);
    if (ImGui::Button(playLabel))
        setPlaying(!playing_);
    if (wasPlaying)
        ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::BeginDisabled(!playing_);
    if (ImGui::Button("  Stop  "))
        setPlaying(false);
    ImGui::EndDisabled();

    ImGui::SameLine(0, 20);
    ImGui::TextDisabled("SCENE: %s   |   ACTORS: %d   |   %s", scene_.name().c_str(),
                        scene_.actorCount(), playing_ ? "PLAYING" : "EDIT MODE");

    ImGui::Dummy(ImVec2(1, 2));
    ImGui::Separator();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Left: hierarchy (with drag & drop reparenting)
// ---------------------------------------------------------------------------
bool EditorApp::acceptActorDrop(Actor* newParent, int index) {
    bool moved = false;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kActorPayload)) {
        uint64_t id = *static_cast<const uint64_t*>(p->Data);
        if (Actor* dragged = findById(scene_.root(), id)) {
            moved = scene_.reparent(dragged, newParent, index, /*keepWorld=*/true);
            if (moved)
                scene_.select(dragged);
        }
    }
    return moved;
}

// A thin horizontal band on the boundary before a row: dropping here inserts the
// dragged actor as a sibling at `insertIndex` under `parent`.
void EditorApp::drawReparentDropTarget(Actor& parent, int insertIndex) {
    const ImGuiPayload* active = ImGui::GetDragDropPayload();
    if (!active || !active->IsDataType(kActorPayload))
        return;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float x0 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
    float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    ImRect band(ImVec2(x0, p.y - 3.0f), ImVec2(x1, p.y + 3.0f));
    ImGuiID tid = ImGui::GetID(&parent) + static_cast<ImGuiID>(insertIndex) + 1u;
    if (ImGui::BeginDragDropTargetCustom(band, tid)) {
        ImGui::GetForegroundDrawList()->AddLine(ImVec2(x0, p.y), ImVec2(x1, p.y),
                                                IM_COL32(0x8B, 0x7C, 0xFF, 0xFF), 2.0f);
        Actor* np = (&parent == &scene_.root()) ? nullptr : &parent;
        acceptActorDrop(np, insertIndex);
        ImGui::EndDragDropTarget();
    }
}

// Returns true if `a` was removed from the scene (caller must stop touching it).
bool EditorApp::hierarchyContextMenu(Actor& a) {
    // Null id -> the popup binds to the last-submitted item (this row), so each
    // row gets its own popup instance.
    static ui::ContextMenu menu(nullptr);
    if (!menu.beginItemPopup())
        return false;
    scene_.select(&a);
    Actor* rootPtr = &scene_.root();
    bool cut = false, del = false; // destructive: applied after the menu closes

    menu.label("EDIT");
    if (menu.item("Cut", "Ctrl+X")) cut = true;
    if (menu.item("Copy", "Ctrl+C")) copyActor(&a);
    if (menu.item("Paste", "Ctrl+V", clipboard_ != nullptr)) scene_.select(pasteInto(&a));
    if (menu.item("Duplicate", "Ctrl+D")) scene_.select(scene_.duplicate(&a));
    if (menu.item("Delete", "Del")) del = true;

    menu.separator();
    menu.label("HIERARCHY");
    if (menu.beginSub("Add New Child")) {
        if (menu.item("Empty Actor")) scene_.select(spawn("actor", &a));
        if (menu.item("3D Actor"))    scene_.select(spawn("actor3d", &a));
        if (menu.item("Mesh"))        scene_.select(spawn("mesh", &a));
        if (menu.item("Sprite"))      scene_.select(spawn("sprite", &a));
        if (menu.item("UI Control"))  scene_.select(spawn("ui", &a));
        if (menu.beginSub("Rendering")) {
            if (menu.item("Camera"))            scene_.select(spawn("camera", &a));
            if (menu.item("Directional Light")) scene_.select(spawn("light_directional", &a));
            if (menu.item("Point Light"))       scene_.select(spawn("light_point", &a));
            if (menu.item("Spot Light"))        scene_.select(spawn("light_spot", &a));
            if (menu.item("Fog"))               scene_.select(spawn("fog", &a));
            if (menu.item("Volumetric Fog"))    scene_.select(spawn("volumetricfog", &a));
            menu.endSub();
        }
        menu.endSub();
    }
    if (menu.item("Reparent To New Node")) reparentToNewNode(&a);
    if (menu.item("Unparent To Root", nullptr, a.parent() != rootPtr))
        scene_.reparent(&a, nullptr, -1, true);

    menu.separator();
    menu.label("STATE");
    if (menu.checkable("Visible", a.visible())) {
        a.setVisible(!a.visible());
        CR_LOG("scene", (a.visible() ? "Showed '" : "Hid '") + a.name() + "'");
    }
    if (menu.checkable("Enabled", a.enabled())) {
        a.setEnabled(!a.enabled());
        CR_LOG("scene", (a.enabled() ? "Enabled '" : "Disabled '") + a.name() + "'");
    }
    menu.end();

    if (cut) {
        cutActor(&a);
        return true;
    }
    if (del) {
        scene_.remove(&a);
        return true;
    }
    return false;
}

void EditorApp::drawHierarchyNode(Actor& actor) {
    ImGui::PushID(static_cast<int>(actor.id()));

    // "Where it will go" boundary target before this row.
    drawReparentDropTarget(*actor.parent(), actor.indexInParent());

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (actor.children().empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (scene_.selected() == &actor)
        flags |= ImGuiTreeNodeFlags_Selected;

    const bool isDragSource = dragActorId_ == actor.id();
    const bool dimmed = isDragSource || !actor.visible() || !actor.enabled();
    // Scene instances are blue (Scenes task spec: "Scene prefabs are blue in
    // color"), so an instanced subtree reads as distinct from plain actors
    // at a glance.
    const bool isInstance = actor.isInstanceRoot();
    if (dimmed)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    else if (isInstance)
        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(0x5A, 0x9C, 0xF5).Value);

    bool open = ImGui::TreeNodeEx(actor.name().c_str(), flags);

    if (dimmed || isInstance)
        ImGui::PopStyleColor();

    // IsItemClicked() fires on mouse-DOWN, before any drag has a chance to
    // register -- selecting here would swap out whatever the Inspector was
    // showing (e.g. a script field you meant to drag this row onto) the
    // instant you pressed the mouse button, not just on an actual click.
    // Select on release-while-still-hovered instead, which a completed drag
    // (released over some other drop target) never satisfies.
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        !ImGui::IsItemToggledOpen())
        scene_.select(&actor);

    // Drag source: carries the actor id, shows a ghost label ("where it was").
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
        uint64_t id = actor.id();
        ImGui::SetDragDropPayload(kActorPayload, &id, sizeof(id));
        dragActorId_ = id;
        ImGui::Text("Move  %s", actor.name().c_str());
        ImGui::EndDragDropSource();
    }

    // Drop ON this row: reparent a dragged actor, or apply a dragged texture /
    // material / mesh to this actor's renderer.
    if (ImGui::BeginDragDropTarget()) {
        acceptActorDrop(&actor, -1);
        auto payloadStr = [](const ImGuiPayload* p) {
            return std::string(static_cast<const char*>(p->Data));
        };
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(pickPayloadId(PickKind::Texture))) {
            std::string tex = payloadStr(p);
            if (auto* mr = actor.getComponent<MeshRenderer>()) {
                mr->texturePath = tex;
                renderer_.invalidateTexture(tex);
                CR_LOG("assets", "Applied texture to '" + actor.name() + "'");
            } else if (auto* sp = dynamic_cast<SpriteActor*>(&actor)) {
                sp->texturePath = tex;
                renderer_.invalidateTexture(tex);
            }
        } else if (const ImGuiPayload* pm =
                       ImGui::AcceptDragDropPayload(pickPayloadId(PickKind::Material))) {
            if (auto* mr = actor.getComponent<MeshRenderer>())
                mr->materialRef = payloadStr(pm);
        }
        ImGui::EndDragDropTarget();
    }

    // Context menu binds to this row (must come before any SameLine item).
    // If it deletes the actor, unwind the ImGui stack and stop.
    if (hierarchyContextMenu(actor)) {
        if (open)
            ImGui::TreePop();
        ImGui::PopID();
        return;
    }

    // Trailing metadata.
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s%s", actor.typeName(), actor.visible() ? "" : "  (hidden)",
                        actor.enabled() ? "" : "  (disabled)");

    if (open) {
        std::vector<Actor*> kids;
        for (const auto& c : actor.children())
            kids.push_back(c.get());
        for (Actor* c : kids)
            drawHierarchyNode(*c);
        // Boundary target after the last child = append under `actor`.
        drawReparentDropTarget(actor, static_cast<int>(actor.children().size()));
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EditorApp::drawHierarchy() {
    if (ImGui::Begin("Hierarchy")) {
        if (ImGui::Button("+ Add")) ImGui::OpenPopup("add_actor");
        {
            static ui::ContextMenu addMenu("add_actor");
            if (addMenu.beginPopup()) {
                Actor* p = scene_.selected();
                if (addMenu.item("Empty Actor")) scene_.select(spawn("actor", p));
                if (addMenu.item("3D Actor"))    scene_.select(spawn("actor3d", p));
                if (addMenu.item("Mesh"))        scene_.select(spawn("mesh", p));
                if (addMenu.item("Sprite"))      scene_.select(spawn("sprite", p));
                if (addMenu.item("UI Control"))  scene_.select(spawn("ui", p));
                if (addMenu.beginSub("Rendering")) {
                    if (addMenu.item("Camera"))            scene_.select(spawn("camera", p));
                    if (addMenu.item("Directional Light")) scene_.select(spawn("light_directional", p));
                    if (addMenu.item("Point Light"))       scene_.select(spawn("light_point", p));
                    if (addMenu.item("Spot Light"))        scene_.select(spawn("light_spot", p));
                    if (addMenu.item("Fog"))               scene_.select(spawn("fog", p));
                    if (addMenu.item("Volumetric Fog"))    scene_.select(spawn("volumetricfog", p));
                    addMenu.endSub();
                }
                addMenu.end();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d objects", scene_.actorCount());
        ImGui::Separator();

        // Reset drag tracking each frame; the source sets it again while active.
        if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            dragActorId_ = 0;

        if (ImGui::BeginChild("tree")) {
            std::vector<Actor*> roots;
            for (const auto& c : scene_.root().children())
                roots.push_back(c.get());
            for (Actor* c : roots)
                drawHierarchyNode(*c);

            // Trailing target under the root (append at end).
            drawReparentDropTarget(scene_.root(), static_cast<int>(scene_.root().children().size()));

            // Empty space below the tree: drop here to unparent to the root.
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.y > 4.0f) {
                ImGui::InvisibleButton("##empty_drop", ImVec2(-1, avail.y));
                if (ImGui::BeginDragDropTarget()) {
                    acceptActorDrop(nullptr, -1);
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::IsItemClicked())
                    scene_.select(nullptr);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Right: inspector
// ---------------------------------------------------------------------------
void EditorApp::drawInspector() {
    if (ImGui::Begin("Inspector")) {
        Actor* a = scene_.selected();

        // A material asset is selected in the Asset Browser: edit it here.
        if (!a && !selectedMaterial_.empty()) {
            Material* mat = materialLib_.find(selectedMaterial_);
            if (!mat) {
                selectedMaterial_.clear();
            } else {
                ImGui::TextDisabled("MATERIAL");
                ImGui::SeparatorText(mat->name.c_str());
                ImGui::ColorEdit4("Base Color", mat->baseColor);
                if (assetPicker_.field("Texture", PickKind::Texture, &mat->texturePath,
                                       pickerSources()))
                    renderer_.invalidateTexture(mat->texturePath);
                ImGui::DragFloat("Emissive", &mat->emissive, 0.01f, 0.0f, 4.0f);
                ImGui::Checkbox("Unlit", &mat->unlit);
                ImGui::Checkbox("Dithered Lighting", &mat->dither);
                if (mat->dither)
                    ImGui::DragFloat("Dither Levels", &mat->ditherLevels, 0.1f, 2.0f, 16.0f);
                ImGui::Spacing();

                static std::string renameBuf;
                if (ImGui::Button("Rename")) {
                    renameBuf = selectedMaterial_;
                    renamePopup_.title("Rename Material")
                        .onBody([](ui::Popup& p) {
                            p.inputText("New name", &renameBuf, /*focusOnAppear=*/true);
                        })
                        .open();
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete Material")) {
                    CR_LOG("assets", "Deleted material '" + selectedMaterial_ + "'");
                    materialLib_.remove(selectedMaterial_);
                    selectedMaterial_.clear();
                }
                if (renamePopup_.draw() == ui::Popup::Result::Ok && !renameBuf.empty()) {
                    if (materialLib_.rename(selectedMaterial_, renameBuf)) {
                        // fix up MeshRenderer references
                        std::function<void(Actor&)> fix = [&](Actor& n) {
                            if (auto* mr = n.getComponent<MeshRenderer>())
                                if (mr->materialRef == selectedMaterial_)
                                    mr->materialRef = renameBuf;
                            for (const auto& c : n.children())
                                fix(*c);
                        };
                        fix(scene_.root());
                        AssetDatabase::get().moved("material:" + selectedMaterial_,
                                                   "material:" + renameBuf);
                        CR_LOG("assets", "Renamed material '" + selectedMaterial_ + "' -> '" +
                                             renameBuf + "'");
                        selectedMaterial_ = renameBuf;
                    }
                }
                ImGui::End();
                return;
            }
        }

        // A script asset is selected in the Asset Browser: edit its
        // namespace here (task 88 -- scripts sharing a namespace will later
        // compile together into one native module).
        if (!a && !selectedScript_.empty()) {
            auto& sys = script::ScriptSystem::get();
            script::ScriptSystem::ScriptFile* sf = sys.file(selectedScript_);
            if (!sf) {
                selectedScript_.clear();
            } else {
                ImGui::TextDisabled("SCRIPT");
                ImGui::SeparatorText(sf->name.c_str());
                if (!sf->error.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Parse error: %s",
                                       sf->error.c_str());

                std::string ns = sys.namespaceOf(sf->name);
                if (ImGui::InputText("Namespace", &ns))
                    sys.setNamespace(sf->name, ns);
                ImGui::TextWrapped(
                    "Scripts sharing a namespace will compile into the same native "
                    "module once script compilation lands.");

                ImGui::Spacing();
                if (ImGui::Button("Open in Script Editor")) {
                    openScriptsTab_ = true;
                    scriptEditor_.openScript(sf->name);
                }
                ImGui::End();
                return;
            }
        }

        if (!a) {
            ImGui::TextDisabled("Nothing selected.");
            ImGui::TextWrapped("Select an actor in the Hierarchy or an asset in the Asset Browser "
                               "to edit its settings here.");
            ImGui::End();
            return;
        }

        std::string name = a->name();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##name", &name))
            a->setName(name);
        ImGui::TextDisabled("%s   |   id %llu", a->typeName(),
                            static_cast<unsigned long long>(a->id()));

        bool vis = a->visible();
        if (ImGui::Checkbox("Visible", &vis)) a->setVisible(vis);
        ImGui::SameLine();
        bool en = a->enabled();
        if (ImGui::Checkbox("Enabled", &en)) a->setEnabled(en);

        ImGui::SeparatorText("Transform");
        Transform& t = a->transform();
        ImGui::DragFloat3("Position", &t.position.x, 0.05f);
        ImGui::DragFloat3("Rotation", &t.rotationEuler.x, 0.5f);
        ImGui::DragFloat3("Scale", &t.scale.x, 0.05f);

        Transform w = a->worldTransform();
        ImGui::TextDisabled("World pos  %.2f, %.2f, %.2f", w.position.x, w.position.y, w.position.z);

        if (auto* sp = dynamic_cast<SpriteActor*>(a)) {
            ImGui::SeparatorText("Sprite");
            assetPicker_.field("Texture", PickKind::Texture, &sp->texturePath, pickerSources());
            ImGui::ColorEdit4("Tint", sp->tint);
        } else if (auto* ui = dynamic_cast<UIControlActor*>(a)) {
            ImGui::SeparatorText("UI Control");
            ImGui::InputText("Label", &ui->label);
            ImGui::DragFloat2("Size", ui->size, 1.0f, 0.0f, 4096.0f);
        }

        ImGui::SeparatorText("Components");
        Component* toRemove = nullptr;
        int ci = 0;
        for (const auto& comp : a->components()) {
            ImGui::PushID(ci++);
            comp->inspectorOpen = ImGui::CollapsingHeader(
                comp->typeName(),
                comp->inspectorOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            if (ImGui::BeginPopupContextItem("comp_ctx")) {
                if (auto* cc = dynamic_cast<CameraComponent*>(comp.get())) {
                    bool camEn = cc->enabled;
                    if (ImGui::Checkbox("Enabled", &camEn)) {
                        if (camEn)
                            activateCamera(*cc);
                        else
                            deactivateCamera(*cc);
                    }
                } else {
                    ImGui::Checkbox("Enabled", &comp->enabled);
                }
                if (ImGui::MenuItem("Remove Component"))
                    toRemove = comp.get();
                ImGui::EndPopup();
            }
            if (comp->inspectorOpen) {
                ImGui::Indent();
                if (!comp->enabled)
                    ImGui::TextDisabled("(disabled)");
                comp->drawInspector();

                // Asset-reference fields for MeshRenderer: drawn here because the
                // component can't reach the asset libraries.
                if (auto* mr = dynamic_cast<MeshRenderer*>(comp.get())) {
                    PickerSources src = pickerSources();
                    if (!mr->usePrimitive) {
                        if (assetPicker_.field("Model", PickKind::Mesh, &mr->meshPath, src) &&
                            !mr->meshPath.empty()) {
                            // if a bare primitive name was picked, switch back
                            static const char* prims[] = {"Cube", "Sphere", "Cylinder",
                                                          "Capsule", "Plane", "Quad"};
                            for (const char* p : prims)
                                if (mr->meshPath == p) {
                                    mr->usePrimitive = true;
                                    mr->primitive = p;
                                    mr->meshPath.clear();
                                }
                        }
                    }
                    assetPicker_.field("Texture", PickKind::Texture, &mr->texturePath, src);
                    if (assetPicker_.field("Material", PickKind::Material, &mr->materialRef, src))
                        renderer_.invalidateTexture(mr->texturePath);
                    if (ImGui::SmallButton("New Material##mrmat")) {
                        Material& m = materialLib_.create("Material");
                        AssetDatabase::get().idFor("material:" + m.name);
                        mr->materialRef = m.name;
                    }
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        if (toRemove) {
            CR_LOG("scene", std::string("Removed ") + toRemove->typeName() + " from '" + a->name() +
                                "'");
            a->removeComponent(toRemove);
        }

        ImGui::Spacing();
        if (ImGui::Button("Add Component", ImVec2(-1, 0)))
            ImGui::OpenPopup("add_component");
        // Right-click blank space in the inspector also opens it.
        if (ImGui::BeginPopupContextWindow("add_component",
                                           ImGuiPopupFlags_MouseButtonRight |
                                               ImGuiPopupFlags_NoOpenOverItems)) {
            std::string lastCat;
            for (const auto& e : ComponentRegistry::get().entries()) {
                if (e.category != lastCat) {
                    ImGui::SeparatorText(e.category.c_str());
                    lastCat = e.category;
                }
                if (ImGui::MenuItem(e.name.c_str())) {
                    Component* c = a->addComponent(ComponentRegistry::get().create(e.name));
                    if (c && playing_)
                        c->start();
                    if (auto* cc = dynamic_cast<CameraComponent*>(c))
                        activateCamera(*cc); // task 77: a newly added camera becomes the active one
                    CR_LOG("scene", "Added " + e.name + " to '" + a->name() + "'");
                }
            }
            ImGui::EndPopup();
        }

        ImGui::SeparatorText("Hierarchy");
        ImGui::Text("Parent: %s", a->parent() ? a->parent()->name().c_str() : "(root)");
        ImGui::Text("Children: %d", static_cast<int>(a->children().size()));
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Middle: viewport
// ---------------------------------------------------------------------------
void EditorApp::drawViewport() {
    if (ImGui::Begin("Viewport")) {
        if (ImGui::BeginTabBar("viewport_tabs")) {
            if (ImGui::BeginTabItem("Scene View")) {
                if (ImGui::RadioButton("Move", gizmoOp_ == 0)) gizmoOp_ = 0;
                ImGui::SameLine();
                if (ImGui::RadioButton("Rotate", gizmoOp_ == 1)) gizmoOp_ = 1;
                ImGui::SameLine();
                if (ImGui::RadioButton("Scale", gizmoOp_ == 2)) gizmoOp_ = 2;
                ImGui::SameLine();
                ImGui::Checkbox("Local", &gizmoLocal_);
                ImGui::SameLine();
                ImGui::Checkbox("Fog", &fog_);
                ImGui::SameLine();
                ImGui::Checkbox("Shadows", &shadows_);
                ImGui::SameLine();
                ImGui::TextDisabled("W/E/R  |  drag = orbit  |  wheel = zoom");

                ImVec2 size = ImGui::GetContentRegionAvail();
                int w = static_cast<int>(size.x), h = static_cast<int>(size.y);
                Renderer::Options opt;
                opt.highlight = scene_.selected();
                opt.fogEnabled = fog_;
                opt.shadows = shadows_;
                void* srv = renderer_.ready() ? renderer_.render(scene_, camera_, w, h, opt) : nullptr;

                if (srv) {
                    ImVec2 imgPos = ImGui::GetCursorScreenPos();
                    ImGui::Image(reinterpret_cast<ImTextureID>(srv), size);
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p =
                                ImGui::AcceptDragDropPayload("CRATE_FBX_PATH"))
                            ingestDroppedFile(static_cast<const char*>(p->Data));
                        else if (const ImGuiPayload* pm =
                                     ImGui::AcceptDragDropPayload(pickPayloadId(PickKind::Mesh))) {
                            std::string key(static_cast<const char*>(pm->Data));
                            auto a = std::make_unique<Actor3D>(fs::path(key).stem().string());
                            auto mr = std::make_unique<MeshRenderer>();
                            mr->usePrimitive = false;
                            mr->meshPath = key;
                            a->addComponent(std::move(mr));
                            scene_.select(scene_.add(std::move(a)));
                        } else if (const ImGuiPayload* ps =
                                       ImGui::AcceptDragDropPayload("CRATE_SCENE_PATH")) {
                            std::string scenePath(static_cast<const char*>(ps->Data));
                            std::string error;
                            if (Actor* inst = scene_.instantiate(scenePath, nullptr, "", &error)) {
                                scene_.select(inst);
                                CR_LOG("scene", "Instantiated '" + scenePath + "' as '" +
                                                    inst->name() + "'");
                            } else {
                                CR_ERROR("scene", "Instantiate failed: " + error);
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    drawViewportOverlays(imgPos.x, imgPos.y, size.x, size.y);
                    drawViewportGizmo(imgPos.x, imgPos.y, size.x, size.y);
                    if (ImGui::IsItemHovered() && !gizmoActive()) {
                        ImGuiIO& io = ImGui::GetIO();
                        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                            camera_.orbit(-io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
                        if (io.MouseWheel != 0.0f)
                            camera_.zoom(io.MouseWheel);
                    }
                } else {
                    ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(0x0B, 0x0D, 0x12, 0xFF));
                    ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 12, p0.y + 12),
                                                       IM_COL32(0x8E, 0x93, 0xA3, 0xFF),
                                                       "Viewport renderer unavailable (no D3D11 device)");
                    ImGui::Dummy(size);
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Game View")) {
                ImVec2 size = ImGui::GetContentRegionAvail();
                int w = static_cast<int>(size.x), h = static_cast<int>(size.y);

                // The active camera (task 77): the one enabled CameraComponent
                // anywhere in the scene, if any. Scene View stays a free-roam
                // OrbitCamera regardless.
                CameraComponent* gameCam = nullptr;
                for (const auto& child : scene_.root().children())
                    if ((gameCam = findCamera(*child, /*requireEnabled=*/true, nullptr)))
                        break;

                void* srv = nullptr;
                if (gameCam && renderer_.ready() && w > 0 && h > 0) {
                    Transform world = gameCam->actor()->worldTransform();
                    Mat4 rot = Mat4::rotationEuler(world.rotationEuler);
                    Vec3 fwd = normalize(Vec3{rot.at(2, 0), rot.at(2, 1), rot.at(2, 2)});
                    Vec3 up = normalize(Vec3{rot.at(1, 0), rot.at(1, 1), rot.at(1, 2)});
                    Vec3 eye = world.position;
                    Mat4 view = Mat4::lookAtLH(eye, eye + fwd, up);
                    float aspect = float(w) / float(h);
                    Mat4 proj = Mat4::perspectiveLH(gameCam->fovY, aspect, gameCam->nearZ,
                                                   gameCam->farZ);
                    Renderer::Options opt;
                    opt.fogEnabled = fog_;
                    opt.shadows = shadows_;
                    srv = renderer_.render(scene_, view, proj, eye, gameCam->nearZ, gameCam->farZ,
                                          w, h, opt);
                }

                if (srv) {
                    ImGui::Image(reinterpret_cast<ImTextureID>(srv), size);
                } else {
                    ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32_BLACK);
                    const char* msg = gameCam ? "Press Play to run the game"
                                              : "No active Camera in the scene";
                    ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 12, p0.y + 12),
                                                        IM_COL32(0xF4, 0xF6, 0xFA, 0xFF), msg);
                    ImGui::Dummy(size);
                }
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags scriptsFlags =
                openScriptsTab_ ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            openScriptsTab_ = false;
            if (ImGui::BeginTabItem("Scripts", nullptr, scriptsFlags)) {
                scriptEditor_.draw();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

bool EditorApp::gizmoActive() const {
    return scene_.selected() && (ImGuizmo::IsOver() || ImGuizmo::IsUsing());
}

namespace {
// Row-vector transform of a point by a row-major matrix (v * M), returning the
// homogeneous result.
struct V4 {
    float x, y, z, w;
};
V4 mulPoint(const Mat4& m, const Vec3& p) {
    V4 o{};
    const float v[4] = {p.x, p.y, p.z, 1.0f};
    o.x = v[0] * m.at(0, 0) + v[1] * m.at(1, 0) + v[2] * m.at(2, 0) + v[3] * m.at(3, 0);
    o.y = v[0] * m.at(0, 1) + v[1] * m.at(1, 1) + v[2] * m.at(2, 1) + v[3] * m.at(3, 1);
    o.z = v[0] * m.at(0, 2) + v[1] * m.at(1, 2) + v[2] * m.at(2, 2) + v[3] * m.at(3, 2);
    o.w = v[0] * m.at(0, 3) + v[1] * m.at(1, 3) + v[2] * m.at(2, 3) + v[3] * m.at(3, 3);
    return o;
}
} // namespace

void EditorApp::drawViewportOverlays(float x, float y, float w, float h) {
    Actor* sel = scene_.selected();
    if (!sel || w < 1.0f || h < 1.0f)
        return;

    const Mat4 viewProj = camera_.view() * camera_.proj(w / h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // world -> screen; returns false when the point is behind the camera.
    auto project = [&](const Vec3& wp, ImVec2& out) -> bool {
        V4 c = mulPoint(viewProj, wp);
        if (c.w <= 1e-4f)
            return false;
        out = ImVec2(x + (c.x / c.w * 0.5f + 0.5f) * w, y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * h);
        return true;
    };
    auto line = [&](const Vec3& a, const Vec3& b, ImU32 col, float thick = 1.5f) {
        ImVec2 pa, pb;
        if (project(a, pa) && project(b, pb))
            dl->AddLine(pa, pb, col, thick);
    };
    auto circle = [&](const Vec3& center, const Vec3& axisU, const Vec3& axisV, float r, ImU32 col) {
        const int N = 40;
        ImVec2 prev;
        bool havePrev = false;
        for (int i = 0; i <= N; ++i) {
            float t = (float)i / N * 2.0f * kPi;
            Vec3 p = center + axisU * (std::cos(t) * r) + axisV * (std::sin(t) * r);
            ImVec2 s;
            bool ok = project(p, s);
            if (ok && havePrev)
                dl->AddLine(prev, s, col, 1.5f);
            prev = s;
            havePrev = ok;
        }
    };

    Transform world = sel->worldTransform();
    Mat4 rot = Mat4::rotationEuler(world.rotationEuler);
    // local axes in world space (row vectors: axis * R = matching row of R).
    Vec3 fwd = normalize(Vec3{rot.at(2, 0), rot.at(2, 1), rot.at(2, 2)});     // +Z
    Vec3 right = normalize(Vec3{rot.at(0, 0), rot.at(0, 1), rot.at(0, 2)});   // +X
    Vec3 up = normalize(Vec3{rot.at(1, 0), rot.at(1, 1), rot.at(1, 2)});      // +Y
    const Vec3 o = world.position;

    // --- Normal arrow: points along the actor's local +Z (task 55) ----------
    {
        const ImU32 col = IM_COL32(90, 200, 255, 220);
        float len = 1.5f;
        Vec3 tip = o + fwd * len;
        line(o, tip, col, 2.0f);
        // arrowhead
        Vec3 back = tip - fwd * (len * 0.22f);
        line(tip, back + right * (len * 0.10f), col, 2.0f);
        line(tip, back - right * (len * 0.10f), col, 2.0f);
        line(tip, back + up * (len * 0.10f), col, 2.0f);
        line(tip, back - up * (len * 0.10f), col, 2.0f);
    }

    // --- Light gizmos (tasks 56, 57) ---------------------------------------
    if (auto* lc = sel->getComponent<LightComponent>()) {
        const ImU32 lcol = IM_COL32(255, 214, 120, 200);
        if (lc->type == LightComponent::Type::Point) {
            circle(o, right, up, lc->range, lcol);
            circle(o, right, fwd, lc->range, lcol);
            circle(o, up, fwd, lc->range, lcol);
        } else if (lc->type == LightComponent::Type::Spot) {
            Vec3 dir = fwd; // light shines along local +Z
            float dist = lc->range;
            float outerR = dist * std::tan(radians(lc->spotOuterDeg));
            float innerR = dist * std::tan(radians(lc->spotInnerDeg));
            Vec3 end = o + dir * dist;
            circle(end, right, up, outerR, lcol);
            circle(end, right, up, innerR, IM_COL32(255, 214, 120, 90));
            for (int i = 0; i < 4; ++i) {
                float a = i * (kPi * 0.5f);
                Vec3 e = end + right * (std::cos(a) * outerR) + up * (std::sin(a) * outerR);
                line(o, e, lcol);
            }
        } else { // directional: a short parallel-ray bundle along +Z
            for (int i = -1; i <= 1; ++i)
                for (int j = -1; j <= 1; ++j) {
                    Vec3 s = o + right * (i * 0.4f) + up * (j * 0.4f);
                    line(s, s + fwd * 1.6f, lcol);
                }
        }
    }

    // --- Camera gizmo (task 77): view frustum wireframe --------------------
    if (auto* cc = sel->getComponent<CameraComponent>()) {
        const ImU32 ccol =
            cc->enabled ? IM_COL32(140, 255, 160, 220) : IM_COL32(150, 150, 150, 150);
        float aspect = w / h;
        float tanH = std::tan(radians(cc->fovY) * 0.5f);
        // Drawn as a pyramid from the actor's own position to the far plane,
        // not a true near-to-far frustum: at nearZ's usual tiny value (0.05),
        // a separate near-plane rectangle sits imperceptibly close to the
        // apex, which visually reads as a stray sliver right at the camera
        // rather than any part of the visible volume worth drawing.
        float farDraw = std::min(cc->farZ, 8.0f);
        auto corner = [&](float dist, float sx, float sy) {
            float hh = tanH * dist;
            float hw = hh * aspect;
            return o + fwd * dist + right * (sx * hw) + up * (sy * hh);
        };
        Vec3 f0 = corner(farDraw, -1, -1), f1 = corner(farDraw, 1, -1),
             f2 = corner(farDraw, 1, 1), f3 = corner(farDraw, -1, 1);
        line(f0, f1, ccol); line(f1, f2, ccol); line(f2, f3, ccol); line(f3, f0, ccol);
        line(o, f0, ccol); line(o, f1, ccol); line(o, f2, ccol); line(o, f3, ccol);
    }
}

// Draw the translate/rotate/scale gizmo over the viewport image for the
// selected actor and fold any manipulation back into its local Transform.
// ImGuizmo's matrix convention (row-major, row vectors, X*Y*Z euler) matches
// the engine's Mat4, so matrices pass through with no transpose.
void EditorApp::drawViewportGizmo(float x, float y, float w, float h) {
    Actor* sel = scene_.selected();
    if (!sel || w < 1.0f || h < 1.0f)
        return;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(x, y, w, h);

    Mat4 view = camera_.view();
    Mat4 proj = camera_.proj(w / h);

    Transform world = sel->worldTransform();
    Mat4 model = Mat4::scale(world.scale) * Mat4::rotationEuler(world.rotationEuler) *
                 Mat4::translation(world.position);

    const ImGuizmo::OPERATION op = gizmoOp_ == 1   ? ImGuizmo::ROTATE
                                   : gizmoOp_ == 2 ? ImGuizmo::SCALE
                                                   : ImGuizmo::TRANSLATE;
    // Scale only makes sense in local space.
    const ImGuizmo::MODE mode =
        (gizmoLocal_ || op == ImGuizmo::SCALE) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    if (ImGuizmo::Manipulate(view.m, proj.m, op, mode, model.m) && ImGuizmo::IsUsing()) {
        float t[3], r[3], s[3];
        ImGuizmo::DecomposeMatrixToComponents(model.m, t, r, s);

        Transform desired;
        desired.position = {t[0], t[1], t[2]};
        desired.rotationEuler = {r[0], r[1], r[2]};
        desired.scale = {s[0], s[1], s[2]};

        Transform parentWorld =
            sel->parent() ? sel->parent()->worldTransform() : Transform{};
        sel->transform() = Transform::localUnder(parentWorld, desired);
    }
}

// ---------------------------------------------------------------------------
// Input Map editor
// ---------------------------------------------------------------------------
namespace {
const char* keyLabel(int k) {
    if (k <= 0)
        return "<unbound>";
    const char* n = ImGui::GetKeyName(static_cast<ImGuiKey>(k));
    return (n && *n) ? n : "<key>";
}
} // namespace

void EditorApp::drawInputMapEditor() {
    // A "Listen" button set inputListenKey_; capture the next key pressed.
    if (inputListenKey_) {
        for (ImGuiKey k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END;
             k = static_cast<ImGuiKey>(k + 1)) {
            if (k >= ImGuiKey_MouseLeft && k <= ImGuiKey_MouseWheelY)
                continue;
            if (ImGui::IsKeyPressed(k, false)) {
                *inputListenKey_ = static_cast<int>(k);
                inputListenKey_ = nullptr;
                break;
            }
        }
    }

    std::string title = "Input Map";
    if (!inputMapPath_.empty())
        title += "  -  " + fs::path(inputMapPath_).filename().string();
    title += "###inputmap";

    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), &inputMapOpen_)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Save") && !inputMapPath_.empty()) {
        inputMap_.save(inputMapPath_);
        Input::get().setMap(&inputMap_);
        CR_LOG("assets", "Saved " + fs::path(inputMapPath_).filename().string());
    }
    ImGui::SameLine();
    ImGui::TextDisabled(inputListenKey_ ? "press any key..." : "map inputs to action names");

    auto listenBtn = [&](const char* id, int* slot) {
        ImGui::PushID(id);
        if (ImGui::SmallButton(inputListenKey_ == slot ? "..." : "Listen"))
            inputListenKey_ = (inputListenKey_ == slot) ? nullptr : slot;
        ImGui::SameLine();
        ImGui::TextUnformatted(keyLabel(*slot));
        ImGui::PopID();
    };

    // --- Buttons: 1 key -> string; signals just_pressed/just_released/pressed.
    if (ImGui::CollapsingHeader("Buttons", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < inputMap_.buttons.size(); ++i) {
            ImGui::PushID((int)i);
            auto& b = inputMap_.buttons[i];
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("##n", &b.name);
            ImGui::SameLine();
            listenBtn("k", &b.key);
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                inputMap_.buttons.erase(inputMap_.buttons.begin() + i);
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ Button"))
            inputMap_.buttons.push_back({"action", 0});
    }

    // --- 2-button axis: neg/pos key -> float.
    if (ImGui::CollapsingHeader("2-Button Axes", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < inputMap_.axes2.size(); ++i) {
            ImGui::PushID(1000 + (int)i);
            auto& a = inputMap_.axes2[i];
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("##n", &a.name);
            ImGui::TextUnformatted("  -");
            ImGui::SameLine();
            listenBtn("neg", &a.negKey);
            ImGui::TextUnformatted("  +");
            ImGui::SameLine();
            listenBtn("pos", &a.posKey);
            if (ImGui::SmallButton("x"))
                inputMap_.axes2.erase(inputMap_.axes2.begin() + i);
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ 2-Axis"))
            inputMap_.axes2.push_back({"axis", 0, 0});
    }

    // --- 4-button axis: L/R/D/U keys -> Vector2.
    if (ImGui::CollapsingHeader("4-Button Axes", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < inputMap_.axes4.size(); ++i) {
            ImGui::PushID(2000 + (int)i);
            auto& a = inputMap_.axes4[i];
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("##n", &a.name);
            ImGui::TextUnformatted("  left");  ImGui::SameLine(); listenBtn("l", &a.leftKey);
            ImGui::TextUnformatted("  right"); ImGui::SameLine(); listenBtn("r", &a.rightKey);
            ImGui::TextUnformatted("  down");  ImGui::SameLine(); listenBtn("d", &a.downKey);
            ImGui::TextUnformatted("  up");    ImGui::SameLine(); listenBtn("u", &a.upKey);
            if (ImGui::SmallButton("x"))
                inputMap_.axes4.erase(inputMap_.axes4.begin() + i);
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ 4-Axis"))
            inputMap_.axes4.push_back({"move", 0, 0, 0, 0});
    }

    // --- Analog: gamepad stick -> Vector2.
    if (ImGui::CollapsingHeader("Analog Axes", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < inputMap_.analogs.size(); ++i) {
            ImGui::PushID(3000 + (int)i);
            auto& a = inputMap_.analogs[i];
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("##n", &a.name);
            ImGui::SameLine();
            const char* sticks[] = {"Left stick", "Right stick"};
            ImGui::SetNextItemWidth(120);
            ImGui::Combo("##s", &a.stick, sticks, 2);
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                inputMap_.analogs.erase(inputMap_.analogs.begin() + i);
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ Analog"))
            inputMap_.analogs.push_back({"look", 0});
    }

    ImGui::Separator();
    ImGui::TextDisabled("Script:  Input.get_button(\"jump\").just_pressed.connect(on_jump)");
    ImGui::TextDisabled("         Input.is_pressed(\"jump\") / Input.get_axis(\"move\")");
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Bottom: asset browser + consoles
// ---------------------------------------------------------------------------
static ImU32 levelColor(Console::Level l) {
    switch (l) {
        case Console::Level::Warn:  return IM_COL32(0xE0, 0xA8, 0x2C, 0xFF);
        case Console::Level::Error: return IM_COL32(0xC0, 0x32, 0x26, 0xFF);
        default:                    return IM_COL32(0xC8, 0xCC, 0xD6, 0xFF);
    }
}

static void drawConsoleChannel(Console::Channel ch) {
    if (ImGui::Button("Clear"))
        Console::get().clear(ch);
    ImGui::SameLine();
    ImGui::TextDisabled("live log");
    ImGui::Separator();
    if (ImGui::BeginChild("log")) {
        for (const auto& e : Console::get().entries()) {
            if (e.channel != ch)
                continue;
            ImGui::TextDisabled("%8.3f", e.time);
            ImGui::SameLine();
            ImGui::TextColored(ImColor(0x8B, 0x7C, 0xFF).Value, "%-8s", e.category.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, levelColor(e.level));
            ImGui::TextUnformatted(e.text.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

unsigned int EditorApp::folderColor(const std::string& relPath) const {
    auto it = folderColors_.find(relPath);
    return it == folderColors_.end() ? 0u : it->second;
}

void EditorApp::loadFolderColors() {
    folderColors_.clear();
    std::ifstream in(fs::path(assetDir_) / ".foldercolors");
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        folderColors_[line.substr(0, tab)] =
            (unsigned int)std::strtoul(line.substr(tab + 1).c_str(), nullptr, 16);
    }
}

void EditorApp::saveFolderColors() {
    std::error_code ec;
    fs::create_directories(assetDir_, ec);
    std::ofstream out(fs::path(assetDir_) / ".foldercolors", std::ios::trunc);
    for (const auto& [p, c] : folderColors_)
        out << p << '\t' << std::hex << c << '\n';
}

void EditorApp::assetBrowserMenu() {
    static ui::ContextMenu menu("asset_ctx");
    if (!menu.beginWindowPopup(/*overItems=*/true))
        return;

    menu.label("CREATE");
    if (menu.item("New Folder")) {
        assetDlg_ = AssetDlg::NewFolder;
        assetDlgTarget_ = assetCwd_;
        assetDlgBuf_ = "New Folder";
        assetPopup_.title("New Folder").onBody([this](ui::Popup& p) {
            p.inputText("Name", &assetDlgBuf_, true);
        }).open();
    }
    if (!folderClip_.path.empty() && menu.item("Paste Folder Here")) {
        std::error_code ec;
        fs::path srcP(folderClip_.path);
        fs::path dst = fs::path(assetDir_) / assetCwd_ / srcP.filename();
        if (folderClip_.cut) {
            fs::rename(srcP, dst, ec);
            if (!ec)
                AssetDatabase::get().movedPrefix(srcP.generic_string() + "/",
                                                 dst.generic_string() + "/");
            folderClip_.path.clear();
        } else {
            fs::copy(srcP, dst, fs::copy_options::recursive, ec);
        }
        CR_LOG("assets", ec ? "Paste failed" : "Pasted folder");
    }
    if (menu.item("Create Material")) {
        Material& m = materialLib_.create("Material");
        AssetDatabase::get().idFor("material:" + m.name);
        selectedMaterial_ = m.name;
        scene_.select(nullptr);
        CR_LOG("assets", "Created material '" + m.name + "'");
    }
    if (menu.item("Create Scene")) {
        std::error_code ec;
        fs::path dir = fs::path(assetDir_) / assetCwd_;
        fs::create_directories(dir, ec);
        fs::path p = dir / "New Scene.cscene";
        for (int n = 2; fs::exists(p, ec); ++n)
            p = dir / ("New Scene " + std::to_string(n) + ".cscene");
        Scene fresh(p.stem().string());
        std::string error;
        if (fresh.save(p.generic_string(), &error)) {
            AssetDatabase::get().idFor(p.generic_string());
            selectedAsset_ = p.generic_string();
            CR_LOG("assets", "Created scene " + p.filename().string());
        } else {
            CR_ERROR("assets", "Create Scene failed: " + error);
        }
    }
    if (menu.item("Create Input Map")) {
        std::error_code ec;
        fs::path dir = fs::path(assetDir_) / assetCwd_;
        fs::create_directories(dir, ec);
        fs::path p = dir / "InputMap.inputmap";
        for (int n = 2; fs::exists(p, ec); ++n)
            p = dir / ("InputMap" + std::to_string(n) + ".inputmap");
        InputMap fresh;
        fresh.buttons.push_back({"jump", 0});
        fresh.save(p.generic_string());
        AssetDatabase::get().idFor(p.generic_string());
        inputMapPath_ = p.generic_string();
        inputMap_ = fresh;
        Input::get().setMap(&inputMap_);
        inputMapOpen_ = true;
        CR_LOG("assets", "Created input map " + p.filename().string());
    }
    if (menu.beginSub("Scripting")) {
        if (menu.item("New Script")) {
            openScriptsTab_ = true;
            scriptEditor_.beginNewScript();
        }
        if (menu.item("Reload Scripts")) {
            script::ScriptSystem::get().reload();
            CR_LOG("script", "Rescanned assets/scripts/");
        }
        menu.endSub();
    }

    menu.separator();
    menu.label("IMPORT");
    if (menu.item("Import Asset...")) {
        std::string picked = platform::openFileDialog(
            "Import Asset",
            "All supported\0*.fbx;*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.tif;*.tiff;*.gif;*.cscript\0"
            "All Files\0*.*\0");
        if (!picked.empty())
            ingestDroppedFile(picked);
    }
    menu.end();
}

// Register every texture file under assets/ so it shows up in the asset picker
// lists and can be dragged onto a component field. Cheap; run on start and
// after an import.
void EditorApp::scanAssets() {
    std::error_code ec;
    if (!fs::exists(assetDir_, ec))
        return;
    for (const auto& e : fs::recursive_directory_iterator(assetDir_, ec)) {
        if (!e.is_regular_file(ec))
            continue;
        std::string path = e.path().generic_string();
        if (!isSupportedImageExt(lowerExt(path)))
            continue;
        if (std::find(importedAssets_.begin(), importedAssets_.end(), path) == importedAssets_.end()) {
            importedAssets_.push_back(path);
            AssetDatabase::get().idFor(path);
        }
    }
}

// Draws the per-type icon glyph into [iconMin, iconMax]: a folder silhouette,
// a shaded sphere, a document with a folded corner and a "C", an isometric
// cube tagged "fbx", a gamepad, or (for images) an actual thumbnail loaded
// through the renderer's texture cache. `accent` is the tile's background
// tint for every kind except Image, which draws directly against a neutral
// backing so the thumbnail's own colours read clearly.
void EditorApp::drawAssetIconGlyph(ImDrawList* dl, ImVec2 iconMin, ImVec2 iconMax,
                                   AssetIconKind kind, unsigned int accent,
                                   const std::string& imagePath) {
    const float w = iconMax.x - iconMin.x, h = iconMax.y - iconMin.y;
    const ImVec2 c((iconMin.x + iconMax.x) * 0.5f, (iconMin.y + iconMax.y) * 0.5f);

    if (kind == AssetIconKind::Image) {
        dl->AddRectFilled(iconMin, iconMax, IM_COL32(20, 22, 28, 255), 6.0f);
        if (void* srv = renderer_.loadTexture(imagePath)) {
            ImVec2 pad(4.0f, 4.0f);
            dl->AddImageRounded(reinterpret_cast<ImTextureID>(srv),
                                ImVec2(iconMin.x + pad.x, iconMin.y + pad.y),
                                ImVec2(iconMax.x - pad.x, iconMax.y - pad.y), ImVec2(0, 0),
                                ImVec2(1, 1), IM_COL32_WHITE, 4.0f);
        }
        return;
    }

    dl->AddRectFilled(iconMin, iconMax, accent, 6.0f);

    switch (kind) {
    case AssetIconKind::Folder: {
        unsigned int fg = IM_COL32(255, 255, 255, 235);
        float bodyTop = iconMin.y + h * 0.34f;
        dl->AddRectFilled(ImVec2(iconMin.x + w * 0.14f, iconMin.y + h * 0.22f),
                          ImVec2(iconMin.x + w * 0.14f + w * 0.42f, bodyTop + 2.0f), fg, 2.0f);
        dl->AddRectFilled(ImVec2(iconMin.x + w * 0.10f, bodyTop),
                          ImVec2(iconMax.x - w * 0.10f, iconMax.y - h * 0.16f), fg, 3.0f);
        break;
    }
    case AssetIconKind::Material: {
        float r = h * 0.30f;
        dl->AddCircleFilled(c, r, IM_COL32(212, 205, 226, 255), 32);
        dl->AddCircle(c, r, IM_COL32(16, 16, 20, 130), 32, 1.5f);
        ImVec2 hl(c.x - r * 0.35f, c.y - r * 0.38f);
        dl->AddCircleFilled(hl, r * 0.34f, IM_COL32(255, 255, 255, 190), 16);
        break;
    }
    case AssetIconKind::Script: {
        float pw = w * 0.5f, ph = h * 0.62f;
        ImVec2 pMin(c.x - pw * 0.5f, c.y - ph * 0.5f), pMax(c.x + pw * 0.5f, c.y + ph * 0.5f);
        dl->AddRectFilled(pMin, pMax, IM_COL32(240, 240, 245, 255), 3.0f);
        float fold = pw * 0.30f;
        ImVec2 f0(pMax.x - fold, pMin.y), f1(pMax.x, pMin.y), f2(pMax.x, pMin.y + fold);
        dl->AddTriangleFilled(f0, f1, f2, IM_COL32(200, 200, 210, 255));
        dl->AddLine(f0, f2, IM_COL32(165, 165, 176, 255), 1.0f);
        const char* letter = "C";
        ImVec2 lsz = ImGui::CalcTextSize(letter);
        dl->AddText(ImVec2(c.x - lsz.x * 0.5f, c.y - lsz.y * 0.5f + ph * 0.06f), accent, letter);
        break;
    }
    case AssetIconKind::Fbx: {
        float s = h * 0.26f, d = h * 0.30f;
        ImVec2 T(c.x, c.y - s - d * 0.15f);
        ImVec2 L(c.x - s * 0.87f, c.y - s * 0.5f - d * 0.15f);
        ImVec2 R(c.x + s * 0.87f, c.y - s * 0.5f - d * 0.15f);
        ImVec2 B(c.x, c.y - d * 0.15f);
        ImVec2 Ld(L.x, L.y + d), Rd(R.x, R.y + d), Bd(B.x, B.y + d);
        ImVec2 top[4] = {T, R, B, L};
        ImVec2 left[4] = {L, B, Bd, Ld};
        ImVec2 right[4] = {R, B, Bd, Rd};
        dl->AddConvexPolyFilled(top, 4, IM_COL32(226, 226, 233, 255));
        dl->AddConvexPolyFilled(left, 4, IM_COL32(166, 166, 179, 255));
        dl->AddConvexPolyFilled(right, 4, IM_COL32(120, 120, 133, 255));
        dl->AddPolyline(top, 4, IM_COL32(18, 18, 22, 140), ImDrawFlags_Closed, 1.0f);
        dl->AddPolyline(left, 4, IM_COL32(18, 18, 22, 140), ImDrawFlags_Closed, 1.0f);
        dl->AddPolyline(right, 4, IM_COL32(18, 18, 22, 140), ImDrawFlags_Closed, 1.0f);
        const char* tag = "fbx";
        ImVec2 tsz = ImGui::CalcTextSize(tag);
        ImVec2 tagMin(iconMax.x - tsz.x - 6.0f, iconMax.y - tsz.y - 4.0f);
        dl->AddRectFilled(tagMin, ImVec2(iconMax.x - 2.0f, iconMax.y - 2.0f),
                          IM_COL32(15, 16, 20, 210), 2.0f);
        dl->AddText(ImVec2(tagMin.x + 2.0f, tagMin.y + 1.0f), IM_COL32(232, 234, 240, 255), tag);
        break;
    }
    case AssetIconKind::InputMap: {
        float bw = w * 0.64f, bh = h * 0.36f;
        ImVec2 bMin(c.x - bw * 0.5f, c.y - bh * 0.5f), bMax(c.x + bw * 0.5f, c.y + bh * 0.5f);
        dl->AddRectFilled(bMin, bMax, IM_COL32(238, 238, 243, 255), bh * 0.5f);
        float padCx = bMin.x + bw * 0.28f, padCy = c.y;
        float armLen = bh * 0.30f, armW = bh * 0.15f;
        dl->AddRectFilled(ImVec2(padCx - armW * 0.5f, padCy - armLen),
                          ImVec2(padCx + armW * 0.5f, padCy + armLen), accent, 1.0f);
        dl->AddRectFilled(ImVec2(padCx - armLen, padCy - armW * 0.5f),
                          ImVec2(padCx + armLen, padCy + armW * 0.5f), accent, 1.0f);
        float btnR = bh * 0.17f;
        dl->AddCircleFilled(ImVec2(bMax.x - bw * 0.22f, c.y - bh * 0.15f), btnR, accent, 12);
        dl->AddCircleFilled(ImVec2(bMax.x - bw * 0.34f, c.y + bh * 0.15f), btnR, accent, 12);
        break;
    }
    case AssetIconKind::Scene: {
        // Small hierarchy glyph (a scene is a tree of actors): one root node
        // with two children, matching the task's "scenes are blue" note via
        // the tile's accent color rather than the glyph itself.
        float r = h * 0.09f;
        ImVec2 root(c.x, iconMin.y + h * 0.30f);
        ImVec2 leftChild(c.x - w * 0.22f, iconMax.y - h * 0.24f);
        ImVec2 rightChild(c.x + w * 0.22f, iconMax.y - h * 0.24f);
        unsigned int line = IM_COL32(235, 235, 240, 200);
        dl->AddLine(root, leftChild, line, 1.5f);
        dl->AddLine(root, rightChild, line, 1.5f);
        dl->AddCircleFilled(root, r * 1.15f, IM_COL32(255, 255, 255, 255), 16);
        dl->AddCircleFilled(leftChild, r, IM_COL32(255, 255, 255, 230), 12);
        dl->AddCircleFilled(rightChild, r, IM_COL32(255, 255, 255, 230), 12);
        break;
    }
    case AssetIconKind::Generic: {
        float pw = w * 0.46f, ph = h * 0.60f;
        ImVec2 pMin(c.x - pw * 0.5f, c.y - ph * 0.5f), pMax(c.x + pw * 0.5f, c.y + ph * 0.5f);
        dl->AddRectFilled(pMin, pMax, IM_COL32(220, 220, 226, 255), 3.0f);
        float fold = pw * 0.30f;
        ImVec2 f0(pMax.x - fold, pMin.y), f1(pMax.x, pMin.y), f2(pMax.x, pMin.y + fold);
        dl->AddTriangleFilled(f0, f1, f2, IM_COL32(190, 190, 198, 255));
        dl->AddLine(f0, f2, IM_COL32(150, 150, 160, 255), 1.0f);
        break;
    }
    default:
        break;
    }
}

// One icon-grid cell: a per-type drawn icon plus a wrapped label underneath,
// sized/selectable like a real asset-browser tile. Everything that used to
// be a text row (folders, files, materials, imported assets) is drawn as one
// of these now; the click/drag/context-menu logic per item is unchanged
// (task 61 - keep functionality, change presentation; task 74 - real icons
// instead of 2-3 letter text glyphs).
bool EditorApp::assetIconTile(const char* strId, AssetIconKind kind, unsigned int accent,
                              const std::string& label, bool selected, bool* dbl,
                              const std::string& imagePath) {
    constexpr float kTileW = 84.0f, kTileH = 96.0f, kIconH = 64.0f;
    ImGui::PushID(strId);
    ImVec2 topLeft = ImGui::GetCursorScreenPos();
    bool clicked =
        ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(kTileW, kTileH));
    if (dbl)
        *dbl = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 iconMin(topLeft.x + 6.0f, topLeft.y + 4.0f);
    ImVec2 iconMax(topLeft.x + kTileW - 6.0f, topLeft.y + 4.0f + kIconH);
    drawAssetIconGlyph(dl, iconMin, iconMax, kind, accent, imagePath);

    ImGui::PushClipRect(topLeft, ImVec2(topLeft.x + kTileW, topLeft.y + kTileH), true);
    ImGui::PushTextWrapPos(topLeft.x + kTileW - 2.0f);
    ImVec2 lsz = ImGui::CalcTextSize(label.c_str(), nullptr, false, kTileW - 4.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
               ImVec2(topLeft.x + (kTileW - (std::min)(lsz.x, kTileW - 4.0f)) * 0.5f,
                      iconMax.y + 4.0f),
               IM_COL32(214, 218, 226, 255), label.c_str(), nullptr, kTileW - 4.0f);
    ImGui::PopTextWrapPos();
    ImGui::PopClipRect();
    ImGui::PopID();
    return clicked;
}

void EditorApp::assetGridWrap(bool moreFollow) {
    if (!moreFollow)
        return;
    float nextX = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + 84.0f;
    if (nextX < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
        ImGui::SameLine();
}

void EditorApp::drawAssetFolders() {
    std::error_code ec;
    fs::path base = fs::path(assetDir_) / assetCwd_;
    if (!fs::exists(base, ec)) {
        ImGui::TextWrapped("No assets folder yet. Right-click to create one.");
        return;
    }

    // Directories first, then files.
    std::vector<fs::directory_entry> dirs, files;
    for (const auto& e : fs::directory_iterator(base, ec)) {
        if (e.path().filename().string()[0] == '.')
            continue; // hidden / sidecar
        (e.is_directory(ec) ? dirs : files).push_back(e);
    }

    for (size_t i = 0; i < dirs.size(); ++i) {
        const auto& d = dirs[i];
        std::string name = d.path().filename().string();
        std::string rel = assetCwd_.empty() ? name : assetCwd_ + "/" + name;
        unsigned int col = folderColor(rel);
        bool dbl = false;
        assetIconTile(rel.c_str(), AssetIconKind::Folder, col ? col : IM_COL32(120, 110, 200, 255),
                     name, false, &dbl);
        if (dbl)
            assetCwd_ = rel;
        folderContextMenu(rel);
        assetGridWrap(i + 1 < dirs.size() || !files.empty());
    }

    for (size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        std::string name = f.path().filename().string();
        std::string path = f.path().generic_string();
        std::string ext = lowerExt(name);
        bool isImg = isSupportedImageExt(ext);
        bool isFbx = ext == "fbx";
        bool isMap = ext == "inputmap";
        bool isScript = ext == "cscript";
        bool isScene = ext == "cscene";
        AssetIconKind kind = isImg      ? AssetIconKind::Image
                            : isFbx     ? AssetIconKind::Fbx
                            : isMap     ? AssetIconKind::InputMap
                            : isScript  ? AssetIconKind::Script
                            : isScene   ? AssetIconKind::Scene
                                        : AssetIconKind::Generic;
        unsigned int col = isImg      ? IM_COL32(70, 150, 170, 255)
                          : isFbx     ? IM_COL32(150, 110, 190, 255)
                          : isMap     ? IM_COL32(90, 130, 200, 255)
                          : isScript  ? IM_COL32(56, 109, 154, 255)
                          : isScene   ? IM_COL32(64, 96, 210, 255)
                                      : IM_COL32(90, 94, 104, 255);
        // Resolve which registered script class (if any) this file currently
        // holds, once per frame, for both the tile's "selected" highlight
        // and click handling below. Compared as fs::path, not raw strings:
        // ScriptSystem stores paths with native (backslash-on-Windows)
        // separators while `path` above is a forward-slash generic_string(),
        // so a plain string == would never match. Matched by resolved class
        // name rather than file stem because a script can be renamed (via
        // the Script Editor) without its backing file being renamed too.
        std::string scriptClassName;
        if (isScript)
            for (const auto& f2 : script::ScriptSystem::get().files())
                if (fs::path(f2.path) == fs::path(path)) { scriptClassName = f2.name; break; }

        bool dbl = false;
        bool clicked = assetIconTile(
            path.c_str(), kind, col, name,
            isScript && !scriptClassName.empty() && selectedScript_ == scriptClassName, &dbl,
            isImg ? path : std::string());
        if (clicked) {
            if (isMap) {
                if (dbl) {
                    inputMapPath_ = path;
                    inputMap_.load(path);
                    Input::get().setMap(&inputMap_);
                    inputMapOpen_ = true;
                }
            } else if (isScript) {
                // Select it in the Inspector (namespace editing, etc.)
                // without touching disk or recompiling anything; a second
                // click within the double-click window additionally opens
                // it in the full Script Editor. Previously ANY click here
                // fell through to ingestDroppedFile(), which re-copied the
                // file onto itself and reloaded every script in the project
                // on every single click.
                if (!scriptClassName.empty()) {
                    selectedScript_ = scriptClassName;
                    selectedMaterial_.clear();
                    selectedAsset_.clear();
                    scene_.select(nullptr);
                    if (dbl) {
                        openScriptsTab_ = true;
                        scriptEditor_.openScript(scriptClassName);
                    }
                }
            } else if (isScene) {
                selectedAsset_ = path;
                selectedMaterial_.clear();
                selectedScript_.clear();
                scene_.select(nullptr);
                if (dbl) {
                    std::string error;
                    Scene loaded = Scene::load(path, &error);
                    if (!error.empty()) {
                        CR_ERROR("scene", "Open failed: " + error);
                    } else {
                        scene_ = std::move(loaded);
                        currentScenePath_ = path;
                        renderer_.loadLightmap(scene_, assetDir_);
                        CR_LOG("scene", "Opened scene " + path);
                    }
                }
            } else {
                ingestDroppedFile(f.path().string());
            }
        }
        // Drag a texture straight onto a component's Texture field.
        if (isImg && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            ImGui::SetDragDropPayload(pickPayloadId(PickKind::Texture), path.c_str(),
                                     path.size() + 1);
            ImGui::Text("Texture  %s", name.c_str());
            ImGui::EndDragDropSource();
            if (std::find(importedAssets_.begin(), importedAssets_.end(), path) ==
                importedAssets_.end())
                importedAssets_.push_back(path);
        }
        // Drag an FBX into the viewport to spawn a mesh actor.
        if (isFbx && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            std::string osPath = f.path().string();
            ImGui::SetDragDropPayload("CRATE_FBX_PATH", osPath.c_str(), osPath.size() + 1);
            ImGui::Text("Model  %s", name.c_str());
            ImGui::EndDragDropSource();
        }
        // Drag a scene into the viewport to place an INSTANCE of it (Scenes
        // task, Step 4: nesting works through the UI too, not just code --
        // this is the natural counterpart to the extraction workflow).
        if (isScene && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            ImGui::SetDragDropPayload("CRATE_SCENE_PATH", path.c_str(), path.size() + 1);
            ImGui::Text("Scene  %s", name.c_str());
            ImGui::EndDragDropSource();
        }
        assetGridWrap(i + 1 < files.size());
    }
}

void EditorApp::folderContextMenu(const std::string& relPath) {
    static ui::ContextMenu menu(nullptr);
    if (!menu.beginItemPopup())
        return;
    std::string name = fs::path(relPath).filename().string();
    if (menu.item("Open"))
        assetCwd_ = relPath;
    menu.separator();
    if (menu.item("Rename")) {
        assetDlg_ = AssetDlg::RenameFolder;
        assetDlgTarget_ = relPath;
        assetDlgBuf_ = name;
        assetPopup_.title("Rename Folder")
            .onBody([this](ui::Popup& p) { p.inputText("Name", &assetDlgBuf_, true); })
            .open();
    }
    if (menu.item("Delete")) {
        assetDlg_ = AssetDlg::DeleteFolder;
        assetDlgTarget_ = relPath;
        assetPopup_.title("Delete Folder")
            .okLabel("Delete")
            .onBody([name](ui::Popup& p) {
                p.help(("Delete '" + name + "' and everything in it? This cannot be undone.")
                           .c_str());
            })
            .open();
    }
    menu.separator();
    if (menu.item("Cut"))
        folderClip_ = {(fs::path(assetDir_) / relPath).string(), true};
    if (menu.item("Copy"))
        folderClip_ = {(fs::path(assetDir_) / relPath).string(), false};
    if (menu.item("Paste Into", nullptr, !folderClip_.path.empty())) {
        std::error_code ec;
        fs::path srcP(folderClip_.path);
        fs::path dst = fs::path(assetDir_) / relPath / srcP.filename();
        if (folderClip_.cut) {
            fs::rename(srcP, dst, ec);
            if (!ec)
                AssetDatabase::get().movedPrefix(srcP.generic_string() + "/",
                                                 dst.generic_string() + "/");
            folderClip_.path.clear();
        } else {
            fs::copy(srcP, dst, fs::copy_options::recursive, ec);
        }
    }
    menu.separator();
    if (menu.item("Change Color...")) {
        assetDlg_ = AssetDlg::ColorFolder;
        assetDlgTarget_ = relPath;
        unsigned int cur = folderColor(relPath);
        ImVec4 c = cur ? ImGui::ColorConvertU32ToFloat4(cur) : ImVec4(0.55f, 0.49f, 1.0f, 1.0f);
        assetDlgColor_[0] = c.x; assetDlgColor_[1] = c.y;
        assetDlgColor_[2] = c.z; assetDlgColor_[3] = 1.0f;
        assetPopup_.title("Folder Color")
            .onBody([this](ui::Popup&) {
                ImGui::ColorPicker3("##col", assetDlgColor_);
            })
            .open();
    }
    if (folderColor(relPath) && menu.item("Clear Color")) {
        folderColors_.erase(relPath);
        saveFolderColors();
    }
    menu.end();
}

void EditorApp::drawAssetPopups() {
    if (assetDlg_ == AssetDlg::None)
        return;
    ui::Popup::Result r = assetPopup_.draw();
    if (r == ui::Popup::Result::Open)
        return;

    std::error_code ec;
    if (r == ui::Popup::Result::Ok) {
        switch (assetDlg_) {
            case AssetDlg::NewFolder:
                if (!assetDlgBuf_.empty())
                    fs::create_directories(
                        fs::path(assetDir_) / assetDlgTarget_ / assetDlgBuf_, ec);
                break;
            case AssetDlg::RenameFolder:
                if (!assetDlgBuf_.empty()) {
                    fs::path p = fs::path(assetDir_) / assetDlgTarget_;
                    fs::path np = p.parent_path() / assetDlgBuf_;
                    fs::rename(p, np, ec);
                    if (!ec)
                        AssetDatabase::get().movedPrefix(p.generic_string() + "/",
                                                         np.generic_string() + "/");
                }
                break;
            case AssetDlg::DeleteFolder:
                fs::remove_all(fs::path(assetDir_) / assetDlgTarget_, ec);
                folderColors_.erase(assetDlgTarget_);
                saveFolderColors();
                if (assetCwd_ == assetDlgTarget_)
                    assetCwd_ = fs::path(assetDlgTarget_).parent_path().string();
                break;
            case AssetDlg::ColorFolder:
                folderColors_[assetDlgTarget_] = ImGui::ColorConvertFloat4ToU32(
                    ImVec4(assetDlgColor_[0], assetDlgColor_[1], assetDlgColor_[2], 1.0f));
                saveFolderColors();
                break;
            default:
                break;
        }
        if (ec)
            CR_WARN("assets", "Folder operation failed: " + ec.message());
    }
    assetDlg_ = AssetDlg::None;
}

void EditorApp::drawBottomPanel() {
    if (ImGui::Begin("Asset Browser")) {
        // Breadcrumb.
        if (ImGui::SmallButton("assets"))
            assetCwd_.clear();
        if (!assetCwd_.empty()) {
            std::string acc;
            for (auto part : fs::path(assetCwd_)) {
                ImGui::SameLine(0, 2);
                ImGui::TextDisabled("/");
                ImGui::SameLine(0, 2);
                acc = acc.empty() ? part.string() : acc + "/" + part.string();
                if (ImGui::SmallButton(part.string().c_str()))
                    assetCwd_ = acc;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("   right-click for actions   |   drag files onto the window");
        ImGui::Separator();

        ImGui::BeginChild("files");
        // Drag an actor from the Hierarchy onto the Asset Browser to turn it
        // (and its children) into a reusable saved scene, replacing it with
        // an instance of that scene (Scenes task, Step 2). An invisible
        // button spanning the whole child is a more reliable drop target
        // than BeginDragDropTarget() called bare right after BeginChild()
        // (which depends on child-window last-item tracking); the cursor is
        // reset afterward so the button sits underneath the real content.
        ImVec2 dropAreaMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##prefab_extract_drop", ImGui::GetContentRegionAvail());
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kActorPayload)) {
                uint64_t id = *static_cast<const uint64_t*>(p->Data);
                if (Actor* dragged = findById(scene_.root(), id)) {
                    extractPrefabTarget_ = dragged;
                    extractPrefabName_ = dragged->name();
                    extractPrefabPopup_.title("Create Scene From Actor")
                        .size(360, 0)
                        .onBody([this](ui::Popup& pop) {
                            pop.help(
                                "Saves this actor and its children as a new scene, then "
                                "replaces it here with an instance of that scene.");
                            pop.inputText("Name", &extractPrefabName_, /*focusOnAppear=*/true);
                        })
                        .open();
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (extractPrefabPopup_.draw() == ui::Popup::Result::Ok && !extractPrefabName_.empty() &&
            extractPrefabTarget_) {
            extractPrefabToScene(extractPrefabTarget_, extractPrefabName_);
            extractPrefabTarget_ = nullptr;
        }
        ImGui::SetCursorScreenPos(dropAreaMin); // draw the real content over the invisible button
        assetBrowserMenu(); // right-click empty space
        drawAssetFolders();

        auto matNames = materialLib_.names();
        for (size_t i = 0; i < matNames.size(); ++i) {
            const std::string& mn = matNames[i];
            if (assetIconTile(("mat:" + mn).c_str(), AssetIconKind::Material,
                              IM_COL32(180, 130, 220, 255), mn, selectedMaterial_ == mn)) {
                selectedMaterial_ = mn;
                selectedAsset_.clear();
                selectedScript_.clear();
                scene_.select(nullptr);
            }
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(pickPayloadId(PickKind::Material), mn.c_str(),
                                         mn.size() + 1);
                ImGui::Text("Material  %s", mn.c_str());
                ImGui::EndDragDropSource();
            }
            assetGridWrap(i + 1 < matNames.size() || !importedAssets_.empty());
        }
        for (size_t i = 0; i < importedAssets_.size(); ++i) {
            const std::string& a = importedAssets_[i];
            std::string name = a;
            if (auto s = name.find_last_of("/\\"); s != std::string::npos)
                name = name.substr(s + 1);
            const std::string ext = lowerExt(a);
            bool isImg = isSupportedImageExt(ext);
            if (assetIconTile(("ia:" + a).c_str(), isImg ? AssetIconKind::Image : AssetIconKind::Fbx,
                              isImg ? IM_COL32(70, 150, 170, 255) : IM_COL32(150, 110, 190, 255),
                              name, selectedAsset_ == a, nullptr, isImg ? a : std::string())) {
                selectedAsset_ = a;
                selectedMaterial_.clear();
                selectedScript_.clear();
                if (isImg) {
                    if (auto* mr = scene_.selected() ? scene_.selected()->getComponent<MeshRenderer>()
                                                     : nullptr) {
                        mr->texturePath = a;
                        renderer_.invalidateTexture(a);
                        CR_LOG("assets",
                               "Applied '" + name + "' to '" + scene_.selected()->name() + "'");
                    }
                }
            }
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(
                    pickPayloadId(isImg ? PickKind::Texture : PickKind::Mesh), a.c_str(),
                    a.size() + 1);
                ImGui::Text("%s  %s", isImg ? "Texture" : "Mesh", name.c_str());
                ImGui::EndDragDropSource();
            }
            assetGridWrap(i + 1 < importedAssets_.size());
        }
        ImGui::EndChild();
        if (ImGui::BeginDragDropTarget()) {
            // Handled by the OS WM_DROPFILES path; this keeps the target visible.
            ImGui::EndDragDropTarget();
        }
        drawAssetPopups();
    }
    ImGui::End();

    if (ImGui::Begin("Engine Console"))
        drawConsoleChannel(Console::Channel::Engine);
    ImGui::End();

    if (ImGui::Begin("Game Console"))
        drawConsoleChannel(Console::Channel::Game);
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
PickerSources EditorApp::pickerSources() {
    PickerSources s;
    s.materials = &materialLib_;
    s.importedAssets = &importedAssets_;
    s.sceneRoot = &scene_.root();
    return s;
}

void EditorApp::deleteSelection() {
    if (Actor* a = scene_.selected()) {
        scene_.remove(a);
        return;
    }
    if (!selectedMaterial_.empty()) {
        CR_LOG("assets", "Deleted material '" + selectedMaterial_ + "'");
        materialLib_.remove(selectedMaterial_);
        AssetDatabase::get().forget("material:" + selectedMaterial_);
        selectedMaterial_.clear();
        return;
    }
    if (!selectedAsset_.empty()) {
        auto it = std::find(importedAssets_.begin(), importedAssets_.end(), selectedAsset_);
        if (it != importedAssets_.end()) {
            CR_LOG("assets", "Removed asset '" + *it + "' from the browser");
            AssetDatabase::get().forget(*it);
            importedAssets_.erase(it);
        }
        selectedAsset_.clear();
    }
}

void EditorApp::saveSceneAs() {
    std::string path = platform::saveFileDialog("Save Scene", "Crate Scene\0*.cscene\0All\0*.*\0",
                                                "cscene");
    if (path.empty())
        return;
    std::string error;
    if (scene_.save(path, &error)) {
        currentScenePath_ = path;
        CR_LOG("scene", "Saved scene to " + path);
    } else {
        CR_ERROR("scene", "Save failed: " + error);
    }
}

void EditorApp::saveScene() {
    if (currentScenePath_.empty()) {
        saveSceneAs();
        return;
    }
    std::string error;
    if (scene_.save(currentScenePath_, &error))
        CR_LOG("scene", "Saved scene to " + currentScenePath_);
    else
        CR_ERROR("scene", "Save failed: " + error);
}

void EditorApp::extractPrefabToScene(Actor* target, const std::string& name) {
    if (!target || !scene_.contains(target))
        return;
    std::error_code ec;
    fs::path dir = fs::path(assetDir_) / assetCwd_;
    fs::create_directories(dir, ec);
    fs::path p = dir / (name + ".cscene");
    for (int n = 2; fs::exists(p, ec); ++n)
        p = dir / (name + " " + std::to_string(n) + ".cscene");

    std::string error;
    if (!scene_.saveSubtree(target, p.generic_string(), &error)) {
        CR_ERROR("assets", "Create Scene From Actor failed: " + error);
        return;
    }
    AssetDatabase::get().idFor(p.generic_string());

    Actor* parent = target->parent();
    if (parent == &scene_.root())
        parent = nullptr;
    int index = target->indexInParent();
    Transform savedTransform = target->transform();
    bool wasVisible = target->visible();
    bool wasEnabled = target->enabled();
    scene_.remove(target); // target is destroyed by this call
    target = nullptr;

    Actor* inst = scene_.instantiate(p.generic_string(), parent, name, &error);
    if (!inst) {
        CR_ERROR("assets", "Create Scene From Actor: instantiate failed: " + error);
        return;
    }
    inst->transform() = savedTransform;
    inst->setVisible(wasVisible);
    inst->setEnabled(wasEnabled);
    scene_.reparent(inst, parent, index, /*keepWorld=*/false); // restore original sibling order
    scene_.select(inst);
    CR_LOG("assets", "Created " + p.filename().string() + " from '" + name +
                         "' and replaced it with an instance");
}

void EditorApp::openScene() {
    std::string path =
        platform::openFileDialog("Open Scene", "Crate Scene\0*.cscene\0All\0*.*\0");
    if (path.empty())
        return;
    std::string error;
    Scene loaded = Scene::load(path, &error);
    if (!error.empty()) {
        CR_ERROR("scene", "Open failed: " + error);
        return;
    }
    scene_ = std::move(loaded);
    currentScenePath_ = path;
    renderer_.loadLightmap(scene_, assetDir_);
    CR_LOG("scene", "Opened scene " + path);
}

void EditorApp::setPlaying(bool playing) {
    if (playing_ == playing)
        return;
    if (playing) {
        // Force-save every unsaved script BEFORE compiling anything -- the
        // on-disk source is what buildNamespace() actually reads (via
        // CodeGen.cpp -> ScriptSystem::types()), so a dirty, unsaved buffer
        // would silently compile the OLD on-disk text otherwise. This is
        // the ONLY point a script ever compiles to native code (see
        // transpiration.txt, "Transplation" Phase 7) -- Play is refused
        // entirely below if any of it fails, so the user always finds out
        // about a compile error before Play visibly starts, never mid-run.
        scriptEditor_.saveAll();

        const std::string scriptsDir = script::ScriptSystem::get().scriptsDir();
        auto dirty = editor::scriptbuild::computeDirtyNamespaces(scriptsDir);
        auto rebuildSet = editor::scriptbuild::computeRebuildSet(dirty, scriptsDir);

        std::vector<editor::scriptbuild::BuildResult> built;
        built.reserve(rebuildSet.size());
        for (const auto& ns : rebuildSet) {
            auto result = editor::scriptbuild::buildNamespace(ns, scriptsDir);
            if (!result.ok) {
                CR_ERROR("script", "namespace '" + ns + "' failed to compile -- Play not started: " +
                                       result.error);
                return; // playing_ stays false: Play never visibly starts
            }
            built.push_back(std::move(result));
        }

        // Every dirty namespace compiled successfully -- load them all
        // (loadNamespace also re-registers namespaces that were ALREADY
        // loaded from a previous Play session, so this is safe to call
        // even for a namespace that didn't need rebuilding this time; only
        // namespaces in rebuildSet are touched here, matching what was
        // actually just (re)built).
        for (const auto& result : built) {
            std::vector<std::string> classNames;
            for (const auto& [name, ci] : script::ScriptSystem::get().types())
                if (script::ScriptSystem::get().namespaceOf(name) == result.namespaceName)
                    classNames.push_back(name);
            if (!script::NativeClassRegistry::get().loadNamespace(result.namespaceName,
                                                                   result.dllPath, classNames)) {
                CR_ERROR("script", "namespace '" + result.namespaceName +
                                       "' compiled but failed to load -- Play not started");
                return;
            }
        }

        scriptEditor_.setPlaying(true);
        playing_ = true;
        playBackup_ = scene_.clone();
        physicsAccum_ = 0.0f;
        scene_.startPlay();
        script::ScriptSystem::get().resetInput();
        Input::get().setMap(inputMapPath_.empty() ? nullptr : &inputMap_);
        script::ScriptSystem::get().startStatics();
        CR_GAME("play", "--- Play started ---");
        CR_LOG("play", "Entered play mode");
    } else {
        Actor* wasSelected = scene_.selected();
        std::string selName = wasSelected ? wasSelected->name() : std::string();
        scene_ = std::move(playBackup_);
        playBackup_ = Scene("");
        scene_.select(nullptr);
        (void)selName;
        // scene_ has now been fully replaced -- every native COMPONENT
        // instance from the just-ended Play session is gone (the
        // moved-from playBackup_ that WAS scene_ is destroyed on
        // reassignment above). Only now is it safe to unload the native
        // modules those instances' code lived in -- see transpiration.txt's
        // Phase 4 ordering-hazard note: never unload a namespace's DLL
        // while a live instance of it still exists anywhere. unloadAll()
        // also restores every affected class's interpreted registration,
        // so Add-Component keeps offering them.
        //
        // unloadAll() MUST run BEFORE this explicit resetStatics() call
        // (Phase 9e), not after: a `static class` singleton's instance is
        // owned by ScriptSystem itself (statics_/nativeStatics_), NOT by
        // the scene tree, so it survives the scene_ reassignment above
        // completely untouched -- if resetStatics() ran first (as it did
        // before Phase 9e, when no static could ever be native), it would
        // find the about-to-be-unloaded namespace's native export STILL
        // registered and rebuild NATIVELY right before unloadAll() yanked
        // that DLL out from under it, leaving a dangling native pointer (a
        // REAL crash caught by static_class_parity_tests.cpp's cleanup
        // path -- see NativeClassRegistry::unloadNamespace(), which now
        // ALSO calls resetStatics() itself, internally, before freeing
        // each namespace's DLL -- this explicit call is now belt-and-
        // braces, guaranteeing every static (not just ones tied to a
        // namespace that happened to unload) gets a truly fresh instance
        // on every Stop).
        script::NativeClassRegistry::get().unloadAll();
        script::ScriptSystem::get().resetStatics();
        script::ScriptSystem::get().resetInput();
        scriptEditor_.setPlaying(false);
        playing_ = false;
        CR_GAME("play", "--- Play stopped ---");
        CR_LOG("play", "Returned to edit mode (scene restored)");
    }
}

void EditorApp::activateCamera(CameraComponent& cam) {
    for (const auto& child : scene_.root().children())
        disableOtherCameras(*child, &cam);
    cam.enabled = true;
}

void EditorApp::deactivateCamera(CameraComponent& cam) {
    cam.enabled = false;
    for (const auto& child : scene_.root().children())
        if (CameraComponent* next = findCamera(*child, /*requireEnabled=*/false, &cam)) {
            next->enabled = true;
            return;
        }
}

Actor* EditorApp::spawn(const char* kind, Actor* parent) {
    std::unique_ptr<Actor> a;
    std::string k = kind;
    if (k == "actor3d") {
        a = std::make_unique<Actor3D>("Actor3D");
    } else if (k == "mesh") {
        a = std::make_unique<Actor3D>("Mesh");
        a->addComponent(std::make_unique<MeshRenderer>()); // defaults to a Cube
    } else if (k == "sprite") {
        a = std::make_unique<SpriteActor>("Sprite");
    } else if (k == "ui") {
        a = std::make_unique<UIControlActor>("UI Control");
    } else if (k == "camera") {
        a = std::make_unique<Actor3D>("Camera");
        a->addComponent(std::make_unique<CameraComponent>());
    } else if (k == "light_directional" || k == "light_point" || k == "light_spot") {
        LightComponent::Type type = k == "light_directional" ? LightComponent::Type::Directional
                                    : k == "light_point"      ? LightComponent::Type::Point
                                                              : LightComponent::Type::Spot;
        a = std::make_unique<Actor3D>(k == "light_directional" ? "Directional Light"
                                     : k == "light_point"       ? "Point Light"
                                                                : "Spot Light");
        auto lc = std::make_unique<LightComponent>();
        lc->type = type;
        a->addComponent(std::move(lc));
    } else if (k == "fog") {
        a = std::make_unique<Actor3D>("Fog");
        a->addComponent(std::make_unique<FogComponent>());
    } else if (k == "volumetricfog") {
        a = std::make_unique<Actor3D>("Volumetric Fog");
        a->addComponent(std::make_unique<VolumetricFogComponent>());
    } else {
        a = std::make_unique<Actor>("Actor");
    }

    std::string name = a->name();
    Actor* added = scene_.add(std::move(a), parent);
    if (auto* cc = added->getComponent<CameraComponent>())
        activateCamera(*cc); // task 77: a newly added camera becomes the active one
    CR_LOG("scene", "Spawned " + name + (parent ? " under '" + parent->name() + "'" : ""));
    return added;
}

void EditorApp::copyActor(Actor* a) {
    if (!scene_.contains(a))
        return;
    clipboard_ = a->clone();
    CR_LOG("edit", "Copied '" + a->name() + "'");
}

void EditorApp::cutActor(Actor* a) {
    if (!scene_.contains(a))
        return;
    clipboard_ = a->clone();
    CR_LOG("edit", "Cut '" + a->name() + "'");
    scene_.remove(a);
}

Actor* EditorApp::pasteInto(Actor* parent) {
    if (!clipboard_)
        return scene_.selected();
    Actor* pasted = scene_.add(clipboard_->clone(), scene_.contains(parent) ? parent : nullptr);
    CR_LOG("edit", "Pasted '" + pasted->name() + "'");
    return pasted;
}

void EditorApp::reparentToNewNode(Actor* a) {
    if (!scene_.contains(a))
        return;
    Actor* parent = a->parent();
    bool parentIsRoot = parent == &scene_.root();
    int idx = a->indexInParent();
    Actor* node = scene_.add(std::make_unique<Actor>("Group"), parentIsRoot ? nullptr : parent, idx);
    scene_.reparent(a, node, -1, true);
    scene_.select(node);
    CR_LOG("scene", "Wrapped '" + a->name() + "' in a new node");
}

} // namespace crate
