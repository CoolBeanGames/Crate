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
    }

    // "Spin preview" slowly orbits the camera so a lone object reads as 3D.
    if (spinPreview_ && !playing_ && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        camera_.yaw += dt * 18.0f;

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
            CR_LOG("scene", "New scene created");
        }
        if (ImGui::MenuItem("Load Sample Scene")) {
            scene_ = Scene::makeSample();
            CR_LOG("scene", "Reloaded sample scene");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S"))
            CR_WARN("scene", "Scene serialization arrives with the Data branch");
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

    const char* playLabel = playing_ ? "  Pause  " : "  Play  ";
    if (playing_)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::SameLine(0, 12);
    if (ImGui::Button(playLabel))
        setPlaying(!playing_);
    if (playing_)
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
    if (dimmed)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

    bool open = ImGui::TreeNodeEx(actor.name().c_str(), flags);

    if (dimmed)
        ImGui::PopStyleColor();

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
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
                ImGui::Checkbox("Enabled", &comp->enabled);
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
                ImGui::Checkbox("Spin preview", &spinPreview_);
                ImGui::SameLine();
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
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32_BLACK);
                const char* msg = playing_ ? "GAME RUNNING" : "Press Play to run the game";
                ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 12, p0.y + 12),
                                                    IM_COL32(0xF4, 0xF6, 0xFA, 0xFF), msg);
                ImGui::Dummy(size);
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

    for (const auto& d : dirs) {
        std::string name = d.path().filename().string();
        std::string rel = assetCwd_.empty() ? name : assetCwd_ + "/" + name;
        unsigned int col = folderColor(rel);
        ImGui::PushID(rel.c_str());
        if (col)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
        bool clicked = ImGui::Selectable(("[dir]  " + name).c_str(), false,
                                         ImGuiSelectableFlags_AllowDoubleClick);
        if (col)
            ImGui::PopStyleColor();
        if (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            assetCwd_ = rel;
        folderContextMenu(rel);
        ImGui::PopID();
    }

    for (const auto& f : files) {
        std::string name = f.path().filename().string();
        std::string path = f.path().generic_string();
        std::string ext = lowerExt(name);
        bool isImg = isSupportedImageExt(ext);
        if (ImGui::Selectable(("       " + name).c_str(), false,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ext == "inputmap") {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    inputMapPath_ = path;
                    inputMap_.load(path);
                    Input::get().setMap(&inputMap_);
                    inputMapOpen_ = true;
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
        if (ext == "fbx" &&
            ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            std::string osPath = f.path().string();
            ImGui::SetDragDropPayload("CRATE_FBX_PATH", osPath.c_str(), osPath.size() + 1);
            ImGui::Text("Model  %s", name.c_str());
            ImGui::EndDragDropSource();
        }
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
        assetBrowserMenu(); // right-click empty space
        drawAssetFolders();

        for (const std::string& mn : materialLib_.names()) {
            if (ImGui::Selectable(("[mat] " + mn).c_str(), selectedMaterial_ == mn)) {
                selectedMaterial_ = mn;
                selectedAsset_.clear();
                scene_.select(nullptr);
            }
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(pickPayloadId(PickKind::Material), mn.c_str(),
                                         mn.size() + 1);
                ImGui::Text("Material  %s", mn.c_str());
                ImGui::EndDragDropSource();
            }
        }
        for (const std::string& a : importedAssets_) {
            std::string name = a;
            if (auto s = name.find_last_of("/\\"); s != std::string::npos)
                name = name.substr(s + 1);
            const std::string ext = lowerExt(a);
            bool isImg = isSupportedImageExt(ext);
            if (ImGui::Selectable(((isImg ? "[img] " : "[mesh] ") + name).c_str(),
                                  selectedAsset_ == a)) {
                selectedAsset_ = a;
                selectedMaterial_.clear();
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

void EditorApp::setPlaying(bool playing) {
    if (playing_ == playing)
        return;
    playing_ = playing;
    if (playing_) {
        scriptEditor_.saveAll(); // auto-save scripts on play
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
        script::ScriptSystem::get().resetStatics();
        script::ScriptSystem::get().resetInput();
        (void)selName;
        CR_GAME("play", "--- Play stopped ---");
        CR_LOG("play", "Returned to edit mode (scene restored)");
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
    } else {
        a = std::make_unique<Actor>("Actor");
    }

    std::string name = a->name();
    Actor* added = scene_.add(std::move(a), parent);
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
