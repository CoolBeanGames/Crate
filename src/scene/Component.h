#pragma once
#include <memory>

namespace crate {

class Actor;

// Components are the building blocks of behaviour and rendering. They attach to
// an Actor and receive lifecycle callbacks while the game is playing:
//
//   start()          - once, when play begins (or when added during play)
//   update(dt)        - every frame,  dt = seconds since the last update
//   physicsUpdate(dt) - every fixed physics step
//
// Concrete components (MeshRenderer, scripts, colliders...) are built on other
// branches and registered with ComponentRegistry so the inspector can add them.
class Component {
public:
    virtual ~Component() = default;

    Actor* actor() const { return actor_; }

    // Stable identifier used by the registry, inspector and serialization.
    virtual const char* typeName() const = 0;

    virtual void start() {}
    virtual void update(float dt) { (void)dt; }
    virtual void physicsUpdate(float dt) { (void)dt; }

    // Draw this component's editable fields in the inspector (ImGui context is
    // already active). Default: nothing.
    virtual void drawInspector() {}

    // Deep copy for actor duplication / paste.
    virtual std::unique_ptr<Component> clone() const = 0;

    bool enabled = true;
    bool inspectorOpen = true;

private:
    friend class Actor;
    Actor* actor_ = nullptr;
};

} // namespace crate
