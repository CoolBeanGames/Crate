#pragma once
#include "script/NativeModule.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace crate::script {

// Runtime registry of every class exported by every currently-loaded native
// script module, keyed by class name -- paralleling crate::ComponentRegistry
// (see scene/ComponentRegistry.h) but specifically tracking the DLLs that
// come out of Phase 3's build pipeline (see transpiration.txt, "Transplation"
// Phase 4). A namespace's classes are ALSO mirrored into
// crate::ComponentRegistry itself (via loadNamespace()) so the Add-Component
// menu and every existing ComponentRegistry::create(name) call site keep
// working completely unchanged, whether a given name resolves to a native
// component or (while not yet loaded) falls through to the interpreted
// ScriptComponent registration ScriptSystem already does.
class NativeClassRegistry {
public:
    static NativeClassRegistry& get();

    // Loads `dllPath` (a crate::editor::scriptbuild::BuildResult::dllPath)
    // and registers every class in `classNames` from it, both in this
    // registry and (replace=true) into crate::ComponentRegistry. If a
    // module was already loaded for `namespaceName`, it is unloaded first
    // (and its ComponentRegistry entries removed) so this is safe to call
    // repeatedly across rebuilds. Returns false, leaving nothing registered
    // for this namespace, if the module fails to load.
    bool loadNamespace(const std::string& namespaceName, const std::string& dllPath,
                       const std::vector<std::string>& classNames);

    // Unloads the module for one namespace (FreeLibrary, and removes its
    // classes from crate::ComponentRegistry). No-op if nothing is loaded
    // for that namespace.
    void unloadNamespace(const std::string& namespaceName);

    // Unloads every currently-loaded native module. Called on Stop (Phase 7)
    // so a subsequent Play always starts from a clean, freshly-built set.
    void unloadAll();

    const NativeClassExport* find(const std::string& className) const;
    bool isLoaded(const std::string& namespaceName) const;

private:
    NativeClassRegistry() = default;
    std::unordered_map<std::string, std::unique_ptr<NativeModule>> modulesByNamespace_;
    // className -> owning namespace, so unloadNamespace()/unloadAll() know
    // which crate::ComponentRegistry entries to remove.
    std::unordered_map<std::string, std::string> namespaceOfClass_;
};

} // namespace crate::script
