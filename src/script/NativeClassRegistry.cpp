#include "script/NativeClassRegistry.h"

#include "scene/ComponentRegistry.h"
#include "script/ScriptSystem.h"

namespace crate::script {
namespace {

// Bridges a native script class's extern "C" factory ABI (which requires
// the owning Actor* up front, see CodeGen.cpp's CreateInstance_<Class>)
// into crate::ComponentRegistry::Entry::make's zero-argument signature
// (`std::function<std::unique_ptr<Component>()>`) -- the same "owner isn't
// known yet at construction time" problem ScriptComponent already solves by
// deferring actual instantiation to first use via Component::actor()
// (inherited, set by Actor::addComponent AFTER this wrapper is constructed
// and returned). Mirrors ScriptComponent::ensureObject()'s lazy-build
// pattern exactly, one level up.
//
// This is a deliberately minimal placeholder for transpiration.txt Phase
// 5's fuller NativeScriptComponent (which will ALSO own an interpreter-
// backed ScriptObject for Inspector-during-Play sync). Kept private to this
// .cpp so Phase 5 is free to replace it outright without any other code
// depending on its shape.
class NativeComponentShim : public crate::Component {
public:
    NativeComponentShim(ScriptContext* ctx, NativeClassExport exp) : ctx_(ctx), exp_(std::move(exp)) {}

    ~NativeComponentShim() override {
        if (native_ && exp_.destroy)
            exp_.destroy(native_);
    }

    const char* typeName() const override { return exp_.className.c_str(); }

    void start() override {
        ensureNative();
        if (native_)
            native_->start();
    }
    void update(float dt) override {
        ensureNative();
        if (native_)
            native_->update(dt);
    }
    void physicsUpdate(float dt) override {
        ensureNative();
        if (native_)
            native_->physicsUpdate(dt);
    }

    std::unique_ptr<crate::Component> clone() const override {
        // Matches ScriptComponent::clone(): a fresh, not-yet-instantiated
        // wrapper -- current field values are not copied, exactly as today.
        return std::make_unique<NativeComponentShim>(ctx_, exp_);
    }

private:
    void ensureNative() {
        if (native_ || !exp_.create)
            return;
        native_ = exp_.create(ctx_, actor());
    }

    ScriptContext* ctx_;
    NativeClassExport exp_;
    crate::Component* native_ = nullptr; // owned; destroyed via exp_.destroy, not `delete`
};

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
        ComponentRegistry::get().add(
            exp.className, "Scripts",
            [captured]() -> std::unique_ptr<crate::Component> {
                // Same ScriptContext every native/interpreted script shares
                // (ScriptSystem owns exactly one, see ScriptSystem::ctx_) --
                // matches how ScriptSystem::registerComponent() captures
                // `&ctx_` for ScriptComponent's own factory lambda.
                return std::make_unique<NativeComponentShim>(&ScriptSystem::get().context(), captured);
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
        ComponentRegistry::get().remove(exp.className);
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
