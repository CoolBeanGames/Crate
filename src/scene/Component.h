#pragma once
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>

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

    // Scene (de)serialization (see SceneIO.cpp): write "key type value..."
    // lines, one per field, via `emit`. Default writes nothing -- an
    // all-default component round-trips fine with zero lines. `idOf` maps a
    // live Actor* to its save-time local id, for a field that references
    // another actor (Value::T::Actor or a native-component live view); most
    // builtin components ignore it, since none of their own fields hold an
    // actor/component reference.
    virtual void writeFields(std::ostream& out, const std::function<int(const Actor*)>& idOf) const {
        (void)out;
        (void)idOf;
    }
    // Restore one field previously written by writeFields(). `key`/`kind`/
    // `value` are exactly one parsed FIELD line (see SceneIO.cpp's format);
    // an unrecognized key is ignored so older/newer scene files stay
    // tolerant of field additions/removals. `actorById` resolves a
    // reference field's saved local id back to a live Actor* (nullptr if
    // out of range / not yet known).
    virtual void readField(const std::string& key, const std::string& kind, const std::string& value,
                           const std::function<Actor*(int)>& actorById) {
        (void)key;
        (void)kind;
        (void)value;
        (void)actorById;
    }

    bool enabled = true;
    bool inspectorOpen = true;

private:
    friend class Actor;
    Actor* actor_ = nullptr;
};

} // namespace crate
