#pragma once
#include "scene/Scene.h"
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
    void drawBottomPanel();     // Asset Browser / Engine Console / Game Console
    void drawHierarchyNode(Actor& actor);

    void setPlaying(bool playing);
    void spawn(const char* kind);

    Scene scene_;
    bool playing_ = false;
    bool showDemo_ = false;
    std::string assetDir_ = "assets";
    char renameBuf_[128] = {0};
};

} // namespace crate
