#include "editor/EditorApp.h"
#include "editor/Console.h"
#include "editor/Theme.h"
#include "scene/Actor2D.h"
#include "scene/Actor3D.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

#include <filesystem>

namespace crate {
namespace fs = std::filesystem;

EditorApp::EditorApp() : scene_(Scene::makeSample()) {
    Console::get().info(Console::Channel::Engine, "Crate editor started.");
    Console::get().info(Console::Channel::Engine,
                        "Loaded sample scene with " + std::to_string(scene_.actorCount()) +
                            " actors.");
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void EditorApp::onFrame() {
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
            Console::get().info(Console::Channel::Engine, "New scene created.");
        }
        if (ImGui::MenuItem("Load Sample Scene")) {
            scene_ = Scene::makeSample();
            Console::get().info(Console::Channel::Engine, "Reloaded sample scene.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            Console::get().warn(Console::Channel::Engine,
                                "Scene serialization arrives with the Data branch.");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Delete Selected", "Del", false, scene_.selected() != nullptr))
            scene_.remove(scene_.selected());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("ImGui Demo", nullptr, &showDemo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Object")) {
        if (ImGui::MenuItem("Create Empty Actor")) spawn("actor");
        if (ImGui::MenuItem("Create 3D Actor"))    spawn("actor3d");
        if (ImGui::MenuItem("Create Mesh"))        spawn("mesh");
        if (ImGui::MenuItem("Create Sprite"))      spawn("sprite");
        if (ImGui::MenuItem("Create UI Control"))  spawn("ui");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        ImGui::TextDisabled("Panels are docked; drag tabs to rearrange.");
        ImGui::EndMenu();
    }

    // Right-aligned settings affordance.
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(ImGui::GetCursorPosX() + w - 90.0f);
    if (ImGui::BeginMenu("Settings")) {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("Frame: %.1f FPS", io.Framerate);
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
// Left: hierarchy
// ---------------------------------------------------------------------------
void EditorApp::drawHierarchyNode(Actor& actor) {
    ImGui::PushID(static_cast<int>(actor.id()));

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (actor.children().empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (scene_.selected() == &actor)
        flags |= ImGuiTreeNodeFlags_Selected;

    bool open = ImGui::TreeNodeEx(actor.name().c_str(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        scene_.select(&actor);

    ImGui::SameLine();
    ImGui::TextDisabled("%s", actor.typeName());

    if (ImGui::BeginPopupContextItem("actor_ctx")) {
        scene_.select(&actor);
        if (ImGui::MenuItem("Add Child Mesh")) {
            Actor* c = scene_.add(std::make_unique<MeshActor>("Mesh"), &actor);
            scene_.select(c);
        }
        if (ImGui::MenuItem("Delete")) {
            ImGui::EndPopup();
            if (open) ImGui::TreePop();
            ImGui::PopID();
            scene_.remove(&actor);
            return;
        }
        ImGui::EndPopup();
    }

    if (open) {
        // Copy child pointers first: deletion during iteration is possible.
        std::vector<Actor*> kids;
        for (const auto& c : actor.children())
            kids.push_back(c.get());
        for (Actor* c : kids)
            drawHierarchyNode(*c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EditorApp::drawHierarchy() {
    if (ImGui::Begin("Hierarchy")) {
        if (ImGui::Button("+ Add")) ImGui::OpenPopup("add_actor");
        if (ImGui::BeginPopup("add_actor")) {
            if (ImGui::MenuItem("Empty Actor")) spawn("actor");
            if (ImGui::MenuItem("3D Actor"))    spawn("actor3d");
            if (ImGui::MenuItem("Mesh"))        spawn("mesh");
            if (ImGui::MenuItem("Sprite"))      spawn("sprite");
            if (ImGui::MenuItem("UI Control"))  spawn("ui");
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d objects", scene_.actorCount());
        ImGui::Separator();

        if (ImGui::BeginChild("tree")) {
            std::vector<Actor*> roots;
            for (const auto& c : scene_.root().children())
                roots.push_back(c.get());
            for (Actor* c : roots)
                drawHierarchyNode(*c);

            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::IsAnyItemHovered())
                scene_.select(nullptr);
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
        if (!a) {
            ImGui::TextDisabled("Nothing selected.");
            ImGui::TextWrapped("Select an actor in the Hierarchy or a file in the Asset Browser "
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
        if (ImGui::Checkbox("Visible", &vis))
            a->setVisible(vis);

        ImGui::SeparatorText("Transform");
        Transform& t = a->transform();
        ImGui::DragFloat3("Position", &t.position.x, 0.05f);
        ImGui::DragFloat3("Rotation", &t.rotationEuler.x, 0.5f);
        ImGui::DragFloat3("Scale", &t.scale.x, 0.05f);

        Transform w = a->worldTransform();
        ImGui::TextDisabled("World pos  %.2f, %.2f, %.2f", w.position.x, w.position.y, w.position.z);

        if (auto* m = dynamic_cast<MeshActor*>(a)) {
            ImGui::SeparatorText("Mesh");
            ImGui::InputText("Mesh Path", &m->meshPath);
            ImGui::InputText("Primitive", &m->primitive);
            ImGui::Checkbox("Cast Shadows", &m->castShadows);
        } else if (auto* sp = dynamic_cast<SpriteActor*>(a)) {
            ImGui::SeparatorText("Sprite");
            ImGui::InputText("Texture Path", &sp->texturePath);
            ImGui::ColorEdit4("Tint", sp->tint);
        } else if (auto* ui = dynamic_cast<UIControlActor*>(a)) {
            ImGui::SeparatorText("UI Control");
            ImGui::InputText("Label", &ui->label);
            ImGui::DragFloat2("Size", ui->size, 1.0f, 0.0f, 4096.0f);
        }

        ImGui::SeparatorText("Hierarchy");
        ImGui::Text("Parent: %s", a->parent() ? a->parent()->name().c_str() : "(root)");
        ImGui::Text("Children: %d", static_cast<int>(a->children().size()));
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Middle: viewport (Scene View / Game View / Scripts)
// ---------------------------------------------------------------------------
void EditorApp::drawViewport() {
    if (ImGui::Begin("Viewport")) {
        if (ImGui::BeginTabBar("viewport_tabs")) {
            if (ImGui::BeginTabItem("Scene View")) {
                ImVec2 size = ImGui::GetContentRegionAvail();
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y),
                                  IM_COL32(0x0B, 0x0D, 0x12, 0xFF));
                // Faint grid so the empty viewport still reads as a 3D space.
                const float step = 32.0f;
                for (float x = 0; x < size.x; x += step)
                    dl->AddLine(ImVec2(p0.x + x, p0.y), ImVec2(p0.x + x, p0.y + size.y),
                                IM_COL32(0x26, 0x2C, 0x38, 0x80));
                for (float y = 0; y < size.y; y += step)
                    dl->AddLine(ImVec2(p0.x, p0.y + y), ImVec2(p0.x + size.x, p0.y + y),
                                IM_COL32(0x26, 0x2C, 0x38, 0x80));
                dl->AddText(ImVec2(p0.x + 12, p0.y + 12), IM_COL32(0x8E, 0x93, 0xA3, 0xFF),
                            "Scene View — renderer lands on the Rendering branch");
                if (Actor* s = scene_.selected())
                    dl->AddText(ImVec2(p0.x + 12, p0.y + 30), IM_COL32(0x8B, 0x7C, 0xFF, 0xFF),
                                ("Selected: " + s->name()).c_str());
                ImGui::Dummy(size);
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
            if (ImGui::BeginTabItem("Scripts")) {
                ImGui::TextWrapped("Script editing binds to the Scripting branch. Selected actor: %s",
                                   scene_.selected() ? scene_.selected()->name().c_str() : "(none)");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
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
            ImGui::PushStyleColor(ImGuiCol_Text, levelColor(e.level));
            ImGui::TextUnformatted(e.text.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void EditorApp::drawBottomPanel() {
    if (ImGui::Begin("Asset Browser")) {
        ImGui::TextDisabled("Browsing: %s", assetDir_.c_str());
        ImGui::Separator();
        if (ImGui::BeginChild("files")) {
            std::error_code ec;
            fs::path base(assetDir_);
            if (!fs::exists(base, ec)) {
                ImGui::TextWrapped("No '%s' folder next to the executable yet. Create one and drop "
                                   "models, textures and sounds in it.",
                                   assetDir_.c_str());
            } else {
                for (const auto& entry : fs::directory_iterator(base, ec)) {
                    const std::string label =
                        (entry.is_directory() ? "[dir] " : "      ") +
                        entry.path().filename().string();
                    if (ImGui::Selectable(label.c_str()))
                        Console::get().info(Console::Channel::Engine,
                                            "Asset picked: " + entry.path().string());
                }
            }
        }
        ImGui::EndChild();
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
void EditorApp::setPlaying(bool playing) {
    if (playing_ == playing)
        return;
    playing_ = playing;
    if (playing_) {
        Console::get().info(Console::Channel::Game, "--- Play started ---");
        Console::get().info(Console::Channel::Engine, "Entered play mode.");
    } else {
        Console::get().info(Console::Channel::Game, "--- Play stopped ---");
        Console::get().info(Console::Channel::Engine, "Returned to edit mode.");
    }
}

void EditorApp::spawn(const char* kind) {
    Actor* parent = scene_.selected();
    std::unique_ptr<Actor> a;
    std::string k = kind;
    if (k == "actor3d")     a = std::make_unique<Actor3D>("Actor3D");
    else if (k == "mesh")   a = std::make_unique<MeshActor>("Mesh");
    else if (k == "sprite") a = std::make_unique<SpriteActor>("Sprite");
    else if (k == "ui")     a = std::make_unique<UIControlActor>("UI Control");
    else                    a = std::make_unique<Actor>("Actor");

    std::string name = a->name();
    Actor* added = scene_.add(std::move(a), parent);
    scene_.select(added);
    Console::get().info(Console::Channel::Engine,
                        "Spawned " + name + (parent ? " under " + parent->name() : ""));
}

} // namespace crate
