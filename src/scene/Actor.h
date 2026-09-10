#pragma once
#include "core/Transform.h"
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

    // Attach an already-owned actor as a child of this one. Returns a raw
    // pointer for convenient selection/inspection; ownership stays in the tree.
    Actor* addChild(std::unique_ptr<Actor> child);

    // Detach a child and return ownership to the caller (nullptr if not found).
    std::unique_ptr<Actor> removeChild(Actor* child);

    // Re-parent this actor under newParent, preserving ownership. Passing the
    // scene root's null is not allowed here; Scene handles root membership.
    void reparent(Actor* newParent);

    bool isDescendantOf(const Actor* other) const;

    void setVisible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }

protected:
    uint64_t id_;
    std::string name_;
    Transform transform_;
    Actor* parent_ = nullptr;
    std::vector<std::unique_ptr<Actor>> children_;
    bool visible_ = true;

private:
    static uint64_t nextId_;
};

} // namespace crate
