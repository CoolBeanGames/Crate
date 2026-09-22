#pragma once
#include "script/ClassInfo.h"
#include "script/Interpreter.h"
#include "script/NativeModule.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crate {
class Scene;
}

namespace crate::script {

// Loads cScript files, keeps the type "dictionary" used for autocomplete, and
// registers each script class as an addable Component.
class ScriptSystem {
public:
    struct ScriptFile {
        std::string path;   // on disk ("" for in-memory)
        std::string name;   // class name
        std::string source; // current text
        std::string error;  // parse error, empty if OK
        bool dirty = false;  // unsaved editor changes
    };

    // A type entry for the editor's autocomplete.
    struct TypeDoc {
        std::string name;
        std::string base;
        bool isScript = false;
        std::vector<std::string> members;   // fields + methods
    };

    static ScriptSystem& get();

    ScriptContext& context() { return ctx_; }

    // Load every *.cscript in a folder (recursively). Safe to call repeatedly;
    // the folder is remembered for newScript() / reload().
    void loadFolder(const std::string& dir);
    const std::string& scriptsDir() const { return dir_; }

    // Drops every loaded script's data (types, namespaces, files, autocomplete
    // docs, retired classes, input-button signals) so a subsequent
    // loadFolder() starts clean instead of merging with a PREVIOUS folder's
    // content (task 92, "Projects": switching the Asset Browser to a
    // different project's scripts folder). Deliberately does NOT touch
    // statics_/nativeStatics_ or any already-loaded native script DLL from a
    // prior Play session -- fully unloading a native module safely is
    // Transplation-level complexity out of scope here; switching projects
    // after having pressed Play earlier in the same editor session is a
    // known limitation (restart the editor for a fully clean switch).
    void unloadAll();

    // Re-scan the scripts folder: new files are loaded, changed files
    // recompiled, deleted files removed from the list / catalogue.
    void reload();

    // Create a new script named `className` from a template in the scripts
    // folder (file <className>.cscript, `class <className> : Actor`), compile
    // and register it. The name is validated (uppercase first letter, no
    // leading digit) and made unique. Returns the final name, or "".
    std::string newScript(const std::string& className = "NewScript");

    // Compile source into a class; registers/updates the type + component.
    // Returns false and fills `errorOut` on a parse error.
    bool compile(const std::string& source, std::string* errorOut, std::string* nameOut = nullptr);

    // Editor helpers.
    std::vector<ScriptFile>& files() { return files_; }
    ScriptFile* file(const std::string& name);
    bool saveFile(ScriptFile& f);
    // Update a script's text, recompile, and (if the class was renamed) retire
    // the old type + menu entry. Returns the script's current name, or "".
    std::string setSource(const std::string& name, std::string source);

    // Autocomplete: all type names, and members of a given type (for `x.` where
    // x is of that type). Also plain keyword/identifier completions.
    const std::vector<TypeDoc>& typeDocs() const { return typeDocs_; }
    std::vector<std::string> completions(const std::string& prefix) const;
    const TypeDoc* typeDoc(const std::string& name) const;

    const std::unordered_map<std::string, std::unique_ptr<ClassInfo>>& types() const {
        return types_;
    }

    // Per-script "namespace": a grouping set on the script asset itself (via
    // the Inspector, not a cScript language construct) that will later
    // decide which compiled DLL a script's native code ends up in -- all
    // scripts sharing a namespace compile together (see transpiration.txt,
    // Phase 1 / Phase 3). Defaults to "Global" when never explicitly set.
    // Persisted to <scripts dir>/.scriptmeta, tab-separated (className<TAB>
    // namespace), mirroring AssetDatabase's own .assetdb convention.
    const std::string& namespaceOf(const std::string& className) const;
    void setNamespace(const std::string& className, std::string ns);

    // Input signal layer: a persistent signal-carrying object per input button,
    // and per-frame dispatch of just_pressed / just_released / pressed to
    // connected callables (call after crate::Input::poll()).
    std::shared_ptr<ScriptObject> inputButton(const std::string& name);
    void dispatchInput();
    void resetInput(); // drop connections (called on Stop)

    // `static class` singletons: one instance each, ticked every frame while
    // playing regardless of any actor.
    void startStatics();
    void tickStatics(float dt);
    void physicsStatics(float dt);
    void resetStatics(); // re-instantiate (called on Stop)

    // Actually performs every Actor.destroy() / Component.remove() call
    // queued (via ctx_.destroyActor/removeComponent) since the last flush
    // -- call once per tick, AFTER every script's update/physics_update has
    // run, so nothing is ever freed while still on the call stack that
    // triggered its own removal. See Interpreter.h's ScriptContext doc.
    // Takes the live Scene so actor destruction goes through Scene::remove()
    // (clears a dangling Inspector selection, logs it) rather than a bare
    // tree-level removeChild().
    void flushPending(crate::Scene& scene);

private:
    ScriptSystem();
    void resolveBases();
    void rebuildTypeDocs();
    void registerComponent(const std::string& className);
    // Destroys/reconstructs statics_[className]'s instance: NATIVE (via
    // NativeClassRegistry::find(className), Phase 9e) if a currently-
    // loaded namespace exports a static-class build for it, else
    // INTERPRETED (Interpreter::instantiate), exactly as before Phase 9e.
    // Always destroys any existing native instance for this class name
    // first (via destroyNativeStaticIfAny), so calling this again after a
    // namespace unloads correctly reverts to interpreted rather than
    // leaving a dangling native pointer.
    void rebuildStatic(const std::string& className);
    void destroyNativeStaticIfAny(const std::string& className);
    void loadNamespaces(); // reads <dir_>/.scriptmeta into namespaces_
    void saveNamespaces() const;

    ScriptContext ctx_;
    std::string dir_; // last folder passed to loadFolder
    std::unordered_map<std::string, std::unique_ptr<ClassInfo>> types_;
    std::unordered_map<std::string, std::string> namespaces_; // className -> namespace
    // ClassInfos for deleted scripts, kept alive so still-attached components
    // don't dangle. Not listed / addable.
    std::vector<std::unique_ptr<ClassInfo>> retired_;
    std::unordered_map<std::string, std::shared_ptr<ScriptObject>> statics_;
    // Owns the raw native instance (Phase 9e) backing a NATIVE static's
    // statics_[className] entry -- statics_ itself only holds the
    // canonical ScriptObject wrapper (selfView_), never the underlying
    // instance, so something has to own destruction; ScriptSystem does,
    // exactly paralleling how NativeScriptComponent owns a Component-
    // shaped instance's lifetime. Empty for an interpreted static.
    struct NativeStaticHandle {
        void* instance = nullptr;
        DestroyStaticFn destroy = nullptr;
    };
    std::unordered_map<std::string, NativeStaticHandle> nativeStatics_;
    std::unordered_map<std::string, std::shared_ptr<ScriptObject>> inputButtons_;
    std::vector<ScriptFile> files_;
    std::vector<TypeDoc> typeDocs_;
    std::vector<crate::Actor*> pendingDestroy_;
    std::vector<std::pair<crate::Actor*, crate::Component*>> pendingComponentRemove_;
};

} // namespace crate::script
