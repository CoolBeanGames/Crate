#include "scene/ComponentRegistry.h"
#include "scene/BuiltinComponents.h"

namespace crate {

ComponentRegistry& ComponentRegistry::get() {
    static ComponentRegistry instance;
    return instance;
}

void ComponentRegistry::add(std::string name, std::string category,
                            std::function<std::unique_ptr<Component>()> make, bool replace) {
    for (auto& e : entries_)
        if (e.name == name) {
            if (replace) {
                e.category = std::move(category);
                e.make = std::move(make);
            }
            return;
        }
    entries_.push_back({std::move(name), std::move(category), std::move(make)});
}

void ComponentRegistry::remove(const std::string& name) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->name == name) {
            entries_.erase(it);
            return;
        }
}

bool ComponentRegistry::has(const std::string& name) const {
    for (const auto& e : entries_)
        if (e.name == name)
            return true;
    return false;
}

std::unique_ptr<Component> ComponentRegistry::create(const std::string& name) const {
    for (const auto& e : entries_)
        if (e.name == name)
            return e.make();
    return nullptr;
}

void registerBuiltinComponents() {
    auto& r = ComponentRegistry::get();
    r.add("Mesh Renderer", "Rendering", [] { return std::make_unique<MeshRenderer>(); });
    r.add("Light", "Rendering", [] { return std::make_unique<LightComponent>(); });
    r.add("Spinner", "Debug", [] { return std::make_unique<SpinnerComponent>(); });
}

} // namespace crate
