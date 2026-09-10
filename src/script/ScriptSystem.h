#pragma once
#include "script/ClassInfo.h"
#include "script/Interpreter.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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

    // Re-scan the scripts folder: new files are loaded, changed files
    // recompiled, deleted files removed from the list / catalogue.
    void reload();

    // Create a new script from a template in the scripts folder, compile and
    // register it. Returns the class/file name, or "" on failure.
    std::string newScript();

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

    // `static class` singletons: one instance each, ticked every frame while
    // playing regardless of any actor.
    void startStatics();
    void tickStatics(float dt);
    void physicsStatics(float dt);
    void resetStatics(); // re-instantiate (called on Stop)

private:
    ScriptSystem();
    void resolveBases();
    void rebuildTypeDocs();
    void registerComponent(const std::string& className);
    void rebuildStatic(const std::string& className);

    ScriptContext ctx_;
    std::string dir_; // last folder passed to loadFolder
    std::unordered_map<std::string, std::unique_ptr<ClassInfo>> types_;
    // ClassInfos for deleted scripts, kept alive so still-attached components
    // don't dangle. Not listed / addable.
    std::vector<std::unique_ptr<ClassInfo>> retired_;
    std::unordered_map<std::string, std::shared_ptr<ScriptObject>> statics_;
    std::vector<ScriptFile> files_;
    std::vector<TypeDoc> typeDocs_;
};

} // namespace crate::script
