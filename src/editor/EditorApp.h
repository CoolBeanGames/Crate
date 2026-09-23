#pragma once
#include "assets/MaterialLibrary.h"
#include "assets/MeshLibrary.h"
#include "editor/AssetPicker.h"
#include "editor/PathRegistry.h"
#include "editor/Popup.h"
#include "editor/ProjectFile.h"
#include "editor/ScriptEditor.h"
#include "input/InputMap.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include <functional>
#include <map>
#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace crate {

class CameraComponent;

// What an asset-browser tile represents, for icon drawing purposes.
enum class AssetIconKind { Folder, Material, Script, Image, Fbx, InputMap, Scene, Generic };

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

    // Opens the project at `crateFilePath` unconditionally (no unsaved-changes
    // guard, no dialog) -- used by main.cpp's `--project <path>` startup
    // argument (task 125, the launcher/`crate open` CLI) to mount a specific
    // project non-interactively before the first frame. The interactive
    // File > Open Project menu item wraps this same logic in the usual
    // requestReplaceScene() guard.
    void openProjectAt(const std::string& crateFilePath);

    // Persists the currently-open project + scene as the "last opened" pair
    // (task 130) so a future bare launch (no --project arg) can resume here
    // instead of the built-in sample scene. main.cpp calls this once at clean
    // shutdown; EditorApp itself also calls it right after opening/creating a
    // project, so at least the project (if not the in-progress scene edits)
    // survives a crash or force-quit. No-op if no project is open (the
    // implicit default project has nothing meaningful to remember).
    void recordLastOpened() const;

    // Draw one editor frame. Call between ImGui::NewFrame() and ImGui::Render().
    void onFrame();

    bool isPlaying() const { return playing_; }

    // "[scene] - [project] - Crate [version]" (task 136). main.cpp polls this
    // once per frame and only calls SetWindowText when it actually changes.
    std::string windowTitle() const;

private:
    void drawMenuBar();
    void drawToolbar();
    void drawHierarchy();
    void drawInspector();
    void drawViewport();
    void drawViewportGizmo(float x, float y, float w, float h); // ImGuizmo overlay
    // Non-interactive overlays: selected actor's +Z normal arrow, and light
    // range / spot-cone wireframes.
    void drawViewportOverlays(float x, float y, float w, float h);
    bool gizmoActive() const; // gizmo hovered or being dragged (suppresses orbit)
    void drawBottomPanel(); // Asset Browser / Engine Console / Game Console
    void assetBrowserMenu(); // right-click menu: create / import / new folder
    void drawAssetFolders(); // navigable folder tree of the assets directory (icon grid)
    // One icon-grid cell: a per-type drawn icon + wrapped label. Returns true
    // on a single click; *dbl is set if that click was a double-click.
    // `imagePath` is only used for AssetIconKind::Image, to load and draw an
    // actual thumbnail instead of a generic glyph. *dblIcon/*dblName (task
    // 138/139) further split *dbl by which half of the tile the double-click
    // landed on -- the icon (top ~kIconH) vs. the name label (below it) --
    // so a caller can tell "double-clicked the icon" (a type-specific open
    // action) apart from "double-clicked the name" (rename) instead of both
    // triggering off the same *dbl.
    bool assetIconTile(const char* strId, AssetIconKind kind, unsigned int accent,
                       const std::string& label, bool selected, bool* dbl = nullptr,
                       const std::string& imagePath = std::string(), bool* dblIcon = nullptr,
                       bool* dblName = nullptr);
    // Draws the icon glyph itself (folder/sphere/paper+C/cube/gamepad/thumbnail)
    // into the given rect; split out of assetIconTile so it only deals with
    // per-type visuals.
    void drawAssetIconGlyph(ImDrawList* dl, ImVec2 iconMin, ImVec2 iconMax, AssetIconKind kind,
                            unsigned int accent, const std::string& imagePath);
    // Call after each tile (with whether more tiles follow) to wrap the grid
    // onto a new row instead of running off the panel's right edge.
    void assetGridWrap(bool moreFollow);
    void scanAssets();       // register image/model files under assets/ as pickable
    void folderContextMenu(const std::string& relPath);
    // Right-click menu for one FILE asset (image/fbx/script/scene -- not
    // input maps, excluded per task 134's own spec) and for one MATERIAL
    // (name-keyed, not file-backed, so it gets its own smaller menu):
    // Rename, Delete (both via the same popup-confirm pattern as
    // folderContextMenu's), Copy, Duplicate.
    void fileAssetContextMenu(const std::string& path);
    void materialContextMenu(const std::string& name);
    // Rename the file at `path` to `newStem` (extension preserved), keeping
    // AssetDatabase/ScriptSystem in sync -- same pattern moveAssetToFolder
    // already established for a cross-folder move, just same-folder.
    void renameFileAsset(const std::string& path, const std::string& newStem);
    // Copy the file at `path` alongside itself under an auto-numbered name
    // ("Name 2.ext", "Name 3.ext", ...), registering it the same way
    // Create Scene/Create Input Map do for a newly made asset.
    void duplicateFileAsset(const std::string& path);
    void drawAssetPopups();  // new folder / rename / delete / colour dialogs
    void drawInputMapEditor(); // double-click an .inputmap -> binding editor window

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

    // Scene file I/O (Scenes task, Phase 1: see scene/SceneIO.cpp). saveScene()
    // reuses currentScenePath_ if the scene was already Saved/Opened this
    // session, else behaves like saveSceneAs() (prompts for a name inline,
    // task 128 -- no OS file dialog). Both are asynchronous when a name
    // prompt is needed: they open the popup and return immediately, and
    // onSaved (if given) fires once the popup is actually confirmed and the
    // write succeeds -- callers that need to sequence work after a save (see
    // drawUnsavedScenePopup) must use the callback, not the return value, to
    // find out when a save-as actually completes.
    void saveSceneAs(std::function<void()> onSaved = nullptr);
    bool saveScene(std::function<void()> onSaved = nullptr); // true only if saved synchronously
                                                              // right now (had a path already)
    void openScene();

    // Project file I/O (task 92, "Projects"): a .crate file marking a folder
    // as a project root, sitting next to that folder's own "assets"
    // subfolder. Minimal on purpose -- see ProjectFile.h and the "for now
    // just get this working" scope note in the Zen task. With no project
    // explicitly opened, the editor behaves exactly as it always has
    // (assetDir_ defaults to "assets" next to the exe/cwd) -- there's always
    // an implicit default project, never a "no project" state.
    void newProject();  // prompts for a new .crate path, creates folder+assets/, mounts it
    void openProject(); // prompts for an existing .crate file; guard + openProjectAt()
    void saveProject(); // re-writes the current project's .crate file; no-op if none is open
    // Re-points assetDir_ at `newAssetDir` and reloads everything keyed off
    // it (AssetDatabase, ScriptSystem, the Asset Browser scan, folder colors,
    // the active input map) as if the editor had started up there. Shared by
    // the constructor (mounts the default project) and by New/Open Project.
    void mountProjectAssets(const std::string& newAssetDir);

    // Unsaved-changes guard (item 106): every action that would replace
    // scene_ wholesale (double-click a .cscene tile, File > Open/New/Load
    // Sample Scene, "Edit Prefab") routes through this instead of mutating
    // scene_ directly. If the current scene has changed since the last
    // load/save it prompts Save/Discard/Cancel first; otherwise `doReplace`
    // runs immediately. `doReplace` is responsible for actually swapping
    // scene_ and updating currentScenePath_/prefabEditReturnPath_.
    void requestReplaceScene(std::function<void()> doReplace);
    void drawUnsavedScenePopup();
    bool hasUnsavedSceneChanges() const;
    void loadSceneNow(const std::string& path); // unconditional load, no guard, no snapshot update

    // Prefab extraction (Scenes task, Step 2): drag an actor onto the Asset
    // Browser -> prompt for a name -> saveSubtree() it as a new .cscene ->
    // remove the original and instantiate() the new scene in its place, at
    // the same parent/sibling position/transform, so nothing visibly moves.
    void extractPrefabToScene(Actor* target, const std::string& name);

    // Moves the file or folder at `srcOsPath` into `destFolderRel` (a
    // folder path relative to assetDir_, "" = the assets root) -- the drop
    // side of every asset tile's CRATE_ASSET_MOVE drag source (item 15/15b:
    // all asset types, and folders themselves, are draggable into a
    // folder). No-ops on a same-location drop or a folder dropped into its
    // own descendant. Keeps AssetDatabase's id tracking correct
    // (moved()/movedPrefix()) and, for a moved script, reloads ScriptSystem
    // so it re-discovers the file at its new path.
    void moveAssetToFolder(const std::string& srcOsPath, const std::string& destFolderRel);

    void setPlaying(bool playing);

    // Camera activation (task 77): enforces "only one CameraComponent in the
    // scene is enabled at a time". activateCamera disables every other one;
    // deactivateCamera disables `cam` and, if another CameraComponent exists
    // anywhere in the tree, activates that one instead.
    void activateCamera(CameraComponent& cam);
    void deactivateCamera(CameraComponent& cam);

    // Delete whatever is currently selected: a scene actor, a material asset,
    // or an imported asset (in that priority order).
    void deleteSelection();

    PickerSources pickerSources();

    Scene scene_;
    Scene playBackup_;      // scene state captured when Play was pressed
    std::string currentScenePath_; // empty until Saved/Opened at least once
    std::string currentProjectPath_; // path to the open .crate file; empty = no
                                       // project explicitly opened (the implicit
                                       // default project: assetDir_ as-is)
    float physicsAccum_ = 0.0f;
    MeshLibrary meshLib_;
    MaterialLibrary materialLib_;
    Renderer renderer_;
    OrbitCamera camera_;
    ScriptEditor scriptEditor_;
    ui::Popup renamePopup_;
    AssetPicker assetPicker_;
    bool openScriptsTab_ = false; // one-shot: select the viewport Scripts tab

    // Viewport transform gizmo (ImGuizmo). op: 0=translate 1=rotate 2=scale.
    int gizmoOp_ = 0;
    bool gizmoLocal_ = true; // local vs world axes
    bool gizmoInit_ = false;
    // Master fog on/off (task 143) -- gates BOTH the plain distance/height Fog
    // component and Volumetric Fog. Defaults on: the sample scene's fog was
    // always visually on regardless of this checkbox before the bug fix, so
    // starting true preserves that default look now that the checkbox
    // actually does something.
    bool fog_ = true;
    bool shadows_ = true;    // viewport real-time shadows

    // Input Map editing.
    InputMap inputMap_;         // the project's active input map
    std::string inputMapPath_;  // file backing inputMap_ ("" = none loaded)
    bool inputMapOpen_ = false; // the binding-editor window is showing
    int* inputListenKey_ = nullptr; // key field currently being captured, or null

    std::vector<std::string> importedAssets_; // keys of imported models/textures
    std::string selectedMaterial_;            // asset selection (inspector shows it)
    std::string selectedAsset_;               // selected imported model/texture path
    std::string selectedScript_;              // selected script asset (class name)

    // Asset-browser folder navigation.
    std::string assetCwd_; // current sub-folder relative to assetDir_ ("" = root)
    std::map<std::string, unsigned int> folderColors_; // relPath -> ImU32 (0xAABBGGRR)
    // Also doubles as the generic asset clipboard for task 134's Copy/Paste
    // (fs::copy/fs::rename don't care whether `path` is a file or a folder).
    struct { std::string path; bool cut = false; } folderClip_;
    enum class AssetDlg {
        None, NewFolder, RenameFolder, DeleteFolder, ColorFolder, SaveScene,
        RenameAsset, DeleteAsset // task 134: shared by file assets AND materials,
                                  // disambiguated by assetDlgIsMaterial_
    };
    AssetDlg assetDlg_ = AssetDlg::None;
    std::string assetDlgTarget_;   // folder relPath / file path / material name the dialog acts on
    std::string assetDlgBuf_;      // name entry
    bool assetDlgIsMaterial_ = false; // RenameAsset/DeleteAsset: assetDlgTarget_ is a
                                       // material name, not a file path
    std::function<void()> saveSceneAsCallback_; // fires once AssetDlg::SaveScene succeeds (task 128)
    float assetDlgColor_[4] = {0.55f, 0.49f, 1.0f, 1.0f};
    ui::Popup assetPopup_;
    ui::Popup extractPrefabPopup_;
    std::string extractPrefabName_;
    Actor* extractPrefabTarget_ = nullptr; // valid only while the popup above is open
    std::unique_ptr<Actor> clipboard_; // deep clone from copy/cut
    bool playing_ = false;
    bool showDemo_ = false;
    bool firstFrame_ = true;
    std::string assetDir_ = "assets";

    // Drag/drop bookkeeping for the hierarchy.
    uint64_t dragActorId_ = 0; // actor currently being dragged (0 = none)

    // Inline rename (task 137): double-click a Hierarchy row's name to edit
    // it in place. 0 = not renaming anything.
    uint64_t renamingActorId_ = 0;
    std::string hierarchyRenameBuf_;

    // Prefab-instance sub-view (item 107): a scene instance's children are
    // hidden inline in the normal tree (it draws as a leaf) until double-
    // clicked, which focuses the Hierarchy panel on that instance -- showing
    // only its own subtree, Godot-style, until "Back" returns to the full
    // scene. nullptr = showing the whole scene as usual.
    Actor* hierarchyFocusRoot_ = nullptr;

    // Unsaved-changes guard + "Edit Prefab" isolation workflow (item 106).
    ui::Popup unsavedScenePopup_;
    std::function<void()> pendingSceneReplace_; // set while unsavedScenePopup_ is open
    enum class PendingSceneAction { Save, Discard };
    PendingSceneAction pendingSceneAction_ = PendingSceneAction::Discard;
    std::string savedSceneSnapshot_; // scene_'s serialized text as of the last load/save
    // Non-empty while editing a .cscene opened via "Edit Prefab" in isolation
    // from the Asset Browser (as opposed to the normal working scene) -- the
    // path to return to, shown as a "< Back to X" banner in the Hierarchy.
    std::string prefabEditReturnPath_;
};

} // namespace crate
