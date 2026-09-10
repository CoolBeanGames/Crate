#pragma once
#include "scene/Component.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace crate {

// Catalogue of component types the editor can add to an actor. Built-in
// components register here at startup (see registerBuiltinComponents).
class ComponentRegistry {
public:
    struct Entry {
        std::string name;                                  // display + identifier
        std::string category;                              // groups the Add menu
        std::function<std::unique_ptr<Component>()> make;
    };

    static ComponentRegistry& get();

    // Register a component type. If `replace` is true an existing entry with
    // the same name is overwritten (used when a script recompiles); otherwise a
    // duplicate name is ignored.
    void add(std::string name, std::string category,
             std::function<std::unique_ptr<Component>()> make, bool replace = false);

    void remove(const std::string& name);
    bool has(const std::string& name) const;

    const std::vector<Entry>& entries() const { return entries_; }
    std::unique_ptr<Component> create(const std::string& name) const;

private:
    std::vector<Entry> entries_;
};

// Registers the engine's built-in components. Call once at startup.
void registerBuiltinComponents();

} // namespace crate
