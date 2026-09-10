#pragma once
#include "assets/MaterialLibrary.h"
#include "assets/MeshLibrary.h"
#include "editor/AssetPicker.h"
#include "editor/Popup.h"
#include "editor/ScriptEditor.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include <map>
#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace crate {

// The editor shell. Owns the active Scene and draws every panel each frame.
// Windowing is handled by the platform layer (see src/main.cpp); the D3D11
// device is created there and handed in so the viewport renderer can share it.
class EditorApp {
public:
    EditorApp();
    ~EditorApp();

    // Provide the shared D3D11 device (enables the 3D viewport). Optional; the
    // editor still runs without it, showing a placeholder viewport.
    void attachDevice(ID3D11Device* device, ID3D11DeviceContext* context);

    // A file dropped onto the window from the OS. FBX files are imported and
    // added to the scene; images are registered for use as textures.
    void ingestDroppedFile(const std::string& path);

    void setScriptMode(bool on) { openScriptsTab_ = on; }

    // Draw one editor frame. Call between ImGui::NewFrame() and ImGui::Render().
    void onFrame();

    bool isPlaying() const { return playing_; }

private:
    void drawMenuBar();
    void drawToolbar();
    void drawHierarchy();
    void drawInspector();
    void drawViewport();
    void drawViewportGizmo(float x, float y, float w, float h); // ImGuizmo overlay
    bool gizmoActive() const; // gizmo hovered or being dragged (suppresses orbit)
    void drawBottomPanel(); // Asset Browser / Engine Console / Game Console
    void assetBrowserMenu(); // right-click menu: create / import / new folder
    void drawAssetFolders(); // navigable folder tree of the assets directory
    void folderContextMenu(const std::string& relPath);
    void drawAssetPopups();  // new folder / rename / delete / colour dialogs

    void loadFolderColors();
    void saveFolderColors();
    unsigned int folderColor(const std::string& relPath) const; // 0 = default

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

    // Delete whatever is currently selected: a scene actor, a material asset,
    // or an imported asset (in that priority order).
    void deleteSelection();

    PickerSources pickerSources();

    Scene scene_;
    Scene playBackup_;      // scene state captured when Play was pressed
    float physicsAccum_ = 0.0f;
    MeshLibrary meshLib_;
    MaterialLibrary materialLib_;
    Renderer renderer_;
    OrbitCamera camera_;
    ScriptEditor scriptEditor_;
    ui::Popup renamePopup_;
    AssetPicker assetPicker_;
    bool openScriptsTab_ = false; // one-shot: select the viewport Scripts tab
    bool spinPreview_ = true;

    // Viewport transform gizmo (ImGuizmo). op: 0=translate 1=rotate 2=scale.
    int gizmoOp_ = 0;
    bool gizmoLocal_ = true; // local vs world axes
    bool gizmoInit_ = false;
    std::vector<std::string> importedAssets_; // keys of imported models/textures
    std::string selectedMaterial_;            // asset selection (inspector shows it)
    std::string selectedAsset_;               // selected imported model/texture path

    // Asset-browser folder navigation.
    std::string assetCwd_; // current sub-folder relative to assetDir_ ("" = root)
    std::map<std::string, unsigned int> folderColors_; // relPath -> ImU32 (0xAABBGGRR)
    struct { std::string path; bool cut = false; } folderClip_;
    enum class AssetDlg { None, NewFolder, RenameFolder, DeleteFolder, ColorFolder };
    AssetDlg assetDlg_ = AssetDlg::None;
    std::string assetDlgTarget_;   // folder relPath the dialog acts on
    std::string assetDlgBuf_;      // name entry
    float assetDlgColor_[4] = {0.55f, 0.49f, 1.0f, 1.0f};
    ui::Popup assetPopup_;
    std::unique_ptr<Actor> clipboard_; // deep clone from copy/cut
    bool playing_ = false;
    bool showDemo_ = false;
    bool firstFrame_ = true;
    std::string assetDir_ = "assets";

    // Drag/drop bookkeeping for the hierarchy.
    uint64_t dragActorId_ = 0; // actor currently being dragged (0 = none)
};

} // namespace crate
