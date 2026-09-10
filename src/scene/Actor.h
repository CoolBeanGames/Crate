#pragma once
#include "core/Transform.h"
#include "scene/Component.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crate {

// Every visible or logical thing in a scene is an Actor: a 3D mesh, a 2D sprite,
// a UI control. Actors own a local Transform and can be parented to one another,
// forming the scene hierarchy. Children inherit their parent's world transform.
//
// Concrete leaf types derive from Actor2D or Actor3D (which derive from Actor).
class Actor {
public:
    explicit Actor(std::string name = "Actor");
    virtual ~Actor() = default;

    Actor(const Actor&) = delete;
    Actor& operator=(const Actor&) = delete;

    uint64_t id() const { return id_; }

    // Stable per-instance UUID (the "AUUID"). Unique across every actor ever
    // constructed in this process, including clones; survives renames and
    // re-parenting. Scene::actorTable() maps current hierarchy paths to these.
    const std::string& auid() const { return auid_; }

    const std::string& name() const { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    // Human-facing type label shown in the hierarchy and inspector (e.g. "ACTOR",
    // "MESH", "SPRITE"). Overridden by concrete types.
    virtual const char* typeName() const { return "ACTOR"; }

    Transform& transform() { return transform_; }
    const Transform& transform() const { return transform_; }

    // Local transform composed up the parent chain.
    Transform worldTransform() const;

    // --- Hierarchy -----------------------------------------------------------
    Actor* parent() const { return parent_; }
    const std::vector<std::unique_ptr<Actor>>& children() const { return children_; }

    // Attach an already-owned actor as a child. `index` < 0 appends; otherwise
    // the child is inserted at that position (clamped). Returns a raw pointer for
    // convenient selection/inspection; ownership stays in the tree.
    Actor* addChild(std::unique_ptr<Actor> child, int index = -1);

    // Detach a child and return ownership to the caller (nullptr if not found).
    std::unique_ptr<Actor> removeChild(Actor* child);

    // Re-parent this actor under newParent, preserving ownership. Scene::reparent
    // is the higher-level entry point (keeps world transform, handles the root).
    void reparent(Actor* newParent);

    // Position of this actor among its parent's children, or -1 if unparented.
    int indexInParent() const;

    bool isDescendantOf(const Actor* other) const;

    // Deep copy of this actor and its whole subtree. A fresh numeric id is
    // always assigned. `preserveAuid` keeps the stable AUUID (used for the
    // play-mode snapshot so Stop restores the same actor identities); the
    // default mints new AUUIDs, since a copy is a new instance.
    std::unique_ptr<Actor> clone(bool preserveAuid = false) const;

    // --- Components --------------------------------------------------------
    const std::vector<std::unique_ptr<Component>>& components() const { return components_; }
    Component* addComponent(std::unique_ptr<Component> c);
    void removeComponent(Component* c);

    template <class T> T* getComponent() const {
        for (const auto& c : components_)
            if (auto* t = dynamic_cast<T*>(c.get()))
                return t;
        return nullptr;
    }

    // Lifecycle fan-out (called by Scene while playing).
    void startComponents();
    void updateComponents(float dt);
    void physicsUpdateComponents(float dt);

    // --- Flags -------------------------------------------------------------
    void setVisible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }

    // Enabled actors tick (Start/Update); disabled ones are inert but still
    // shown in the hierarchy. Independent of visibility.
    void setEnabled(bool v) { enabled_ = v; }
    bool enabled() const { return enabled_; }

protected:
    // Create a copy of just this node (own type + own fields, no children,
    // fresh id). Overridden by every concrete type.
    virtual Actor* cloneSelf() const { return new Actor(name_); }

    uint64_t id_;
    std::string auid_;
    std::string name_;
    Transform transform_;
    Actor* parent_ = nullptr;
    std::vector<std::unique_ptr<Actor>> children_;
    std::vector<std::unique_ptr<Component>> components_;
    bool visible_ = true;
    bool enabled_ = true;

private:
    static uint64_t nextId_;
};

} // namespace crate
