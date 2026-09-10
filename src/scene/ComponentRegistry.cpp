#include "scene/ComponentRegistry.h"
#include "scene/BuiltinComponents.h"

namespace crate {

ComponentRegistry& ComponentRegistry::get() {
    static ComponentRegistry instance;
    return instance;
}

void ComponentRegistry::add(std::string name, std::string category,
                            std::function<std::unique_ptr<Component>()> make) {
    for (auto& e : entries_)
        if (e.name == name)
            return; // already registered
    entries_.push_back({std::move(name), std::move(category), std::move(make)});
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
    r.add("Spinner", "Debug", [] { return std::make_unique<SpinnerComponent>(); });
}

} // namespace crate
