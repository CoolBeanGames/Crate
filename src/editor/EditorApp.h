#pragma once
#include "scene/Scene.h"
#include <memory>
#include <string>

namespace crate {

// The editor shell. Owns the active Scene and draws every panel each frame.
// Rendering/windowing is handled by the platform layer (see src/main.cpp); this
// class only issues ImGui calls and is backend-agnostic.
class EditorApp {
public:
    EditorApp();

    // Draw one editor frame. Call between ImGui::NewFrame() and ImGui::Render().
    void onFrame();

    bool isPlaying() const { return playing_; }

private:
    void drawMenuBar();
    void drawToolbar();
    void drawHierarchy();
    void drawInspector();
    void drawViewport();
    void drawBottomPanel(); // Asset Browser / Engine Console / Game Console

    // Hierarchy internals.
    void drawHierarchyNode(Actor& actor);
    void drawReparentDropTarget(Actor& parent, int insertIndex); // thin line between rows
    bool hierarchyContextMenu(Actor& actor); // returns true if the actor was deleted
    bool acceptActorDrop(Actor* newParent, int index); // returns true if a drop happened

    // Scene actions (all logged, all undo-friendly in spirit).
    Actor* spawn(const char* kind, Actor* parent);
    void copyActor(Actor* a);
    void cutActor(Actor* a);
    Actor* pasteInto(Actor* parent);
    void reparentToNewNode(Actor* a);

    void setPlaying(bool playing);

    Scene scene_;
    std::unique_ptr<Actor> clipboard_; // deep clone from copy/cut
    bool playing_ = false;
    bool showDemo_ = false;
    bool firstFrame_ = true;
    std::string assetDir_ = "assets";

    // Drag/drop bookkeeping for the hierarchy.
    uint64_t dragActorId_ = 0; // actor currently being dragged (0 = none)
};

} // namespace crate
