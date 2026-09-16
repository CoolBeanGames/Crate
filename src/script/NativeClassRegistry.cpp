#include "script/NativeClassRegistry.h"

#include "scene/ComponentRegistry.h"
#include "script/NativeScriptComponent.h"
#include "script/ScriptComponent.h"
#include "script/ScriptSystem.h"

namespace crate::script {
namespace {
// Re-registers `className` in ComponentRegistry against the INTERPRETED
// ScriptComponent factory -- i.e. restores exactly what
// ScriptSystem::registerComponent() already set up before any namespace was
// ever built/loaded. Used by unloadNamespace()/unloadAll() so unloading a
// namespace's DLL (Stop, or before a rebuild) returns the editor to normal
// edit-time behavior (the class stays addable, running interpreted) rather
// than vanishing from the Add-Component menu entirely, which would happen
// if this only called ComponentRegistry::remove(). Falls back to an actual
// remove() only if the class no longer exists in ScriptSystem at all (its
// script file was deleted, not just its namespace's DLL unloaded).
void restoreInterpretedRegistration(const std::string& className) {
    auto typeIt = ScriptSystem::get().types().find(className);
    if (typeIt == ScriptSystem::get().types().end()) {
        ComponentRegistry::get().remove(className);
        return;
    }
    ScriptContext* ctx = &ScriptSystem::get().context();
    const ClassInfo* cls = typeIt->second.get();
    ComponentRegistry::get().add(
        className, "Scripts", [ctx, cls] { return std::make_unique<ScriptComponent>(ctx, cls); },
        /*replace=*/true);
}
} // namespace

NativeClassRegistry& NativeClassRegistry::get() {
    static NativeClassRegistry instance;
    return instance;
}

bool NativeClassRegistry::loadNamespace(const std::string& namespaceName, const std::string& dllPath,
                                        const std::vector<std::string>& classNames) {
    unloadNamespace(namespaceName); // safe to call repeatedly across rebuilds

    NativeModule mod = NativeModule::load(dllPath, classNames);
    if (!mod.ok())
        return false;

    auto owned = std::make_unique<NativeModule>(std::move(mod));
    for (const auto& exp : owned->exports()) {
        namespaceOfClass_[exp.className] = namespaceName;
        NativeClassExport captured = exp; // by-value capture: stable across further loads/unloads
        // The interpreted ClassInfo* for this same class name always exists
        // -- ScriptSystem::compile() runs for every loaded script
        // regardless of namespace (Phase 1 only adds a metadata tag, it
        // never changes what ScriptSystem itself compiles) -- and pointer
        // identity is stable across recompiles/retirement forever (see
        // ScriptSystem.cpp's compile()/retired_), so capturing it directly
        // is exactly as safe as ScriptComponent already assumes.
        auto typeIt = ScriptSystem::get().types().find(exp.className);
        const ClassInfo* cls = typeIt != ScriptSystem::get().types().end() ? typeIt->second.get() : nullptr;
        ComponentRegistry::get().add(
            exp.className, "Scripts",
            [captured, cls]() -> std::unique_ptr<crate::Component> {
                // Same ScriptContext every native/interpreted script shares
                // (ScriptSystem owns exactly one, see ScriptSystem::ctx_) --
                // matches how ScriptSystem::registerComponent() captures
                // `&ctx_` for ScriptComponent's own factory lambda.
                return std::make_unique<NativeScriptComponent>(&ScriptSystem::get().context(), cls,
                                                                captured);
            },
            /*replace=*/true);
    }
    modulesByNamespace_[namespaceName] = std::move(owned);
    return true;
}

void NativeClassRegistry::unloadNamespace(const std::string& namespaceName) {
    auto it = modulesByNamespace_.find(namespaceName);
    if (it == modulesByNamespace_.end())
        return;
    for (const auto& exp : it->second->exports()) {
        restoreInterpretedRegistration(exp.className);
        namespaceOfClass_.erase(exp.className);
    }
    modulesByNamespace_.erase(it); // ~NativeModule() FreeLibrary()s here
}

void NativeClassRegistry::unloadAll() {
    std::vector<std::string> names;
    names.reserve(modulesByNamespace_.size());
    for (const auto& [ns, mod] : modulesByNamespace_)
        names.push_back(ns);
    for (const auto& ns : names)
        unloadNamespace(ns);
}

const NativeClassExport* NativeClassRegistry::find(const std::string& className) const {
    auto nsIt = namespaceOfClass_.find(className);
    if (nsIt == namespaceOfClass_.end())
        return nullptr;
    auto modIt = modulesByNamespace_.find(nsIt->second);
    if (modIt == modulesByNamespace_.end())
        return nullptr;
    return modIt->second->find(className);
}

bool NativeClassRegistry::isLoaded(const std::string& namespaceName) const {
    return modulesByNamespace_.count(namespaceName) != 0;
}

} // namespace crate::script
