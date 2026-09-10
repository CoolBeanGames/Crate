#pragma once
#include "scene/Actor.h"
#include <string>
#include <utility>
#include <vector>

namespace crate {

// A Scene owns a single implicit root Actor; everything the user adds becomes a
// child of that root. Selection is tracked here so the editor panels share one
// source of truth.
class Scene {
public:
    Scene(std::string name = "Untitled");

    const std::string& name() const { return name_; }
    void setName(std::string n) { name_ = std::move(n); }

    Actor& root() { return *root_; }

    // Add a new actor under `parent` (or the root when parent is null),
    // optionally at a specific sibling index (-1 appends).
    Actor* add(std::unique_ptr<Actor> actor, Actor* parent = nullptr, int index = -1);

    // Remove an actor (and its subtree) from the scene entirely.
    void remove(Actor* actor);

    // Move `actor` under `newParent` (null = root) at `index` (-1 appends).
    // When keepWorld is true the local transform is recomputed so the actor
    // does not visibly move. No-op (returns false) if the move would form a
    // cycle. Dropping onto its current position is allowed (reorder).
    bool reparent(Actor* actor, Actor* newParent, int index = -1, bool keepWorld = true);

    // Deep-copy `actor` and insert the copy as its next sibling. Returns the
    // copy, or nullptr if `actor` is not in the scene.
    Actor* duplicate(Actor* actor);

    // True when `actor` belongs to this scene's tree (not the root itself).
    bool contains(const Actor* actor) const;

    Actor* selected() const { return selected_; }
    void select(Actor* actor) { selected_ = actor; }

    // Total actor count excluding the root.
    int actorCount() const;

    // --- Actor paths / AUUIDs ------------------------------------------------
    // Slash-joined actor names from the root down to `actor` (root excluded),
    // e.g. "SET/Crate". Empty if `actor` is null or the root. Sibling name
    // clashes are disambiguated with a "#n" suffix on the later sibling.
    std::string pathOf(const Actor* actor) const;

    // The actor at a path produced by pathOf(), or nullptr.
    Actor* atPath(const std::string& path) const;

    // Current hierarchy path -> actor AUUID, for every actor in the scene.
    // Recomputed from the live tree, so it always reflects the latest moves.
    std::vector<std::pair<std::string, std::string>> actorTable() const;

    // --- Play-mode ticking ----------------------------------------------
    void startPlay() { root_->startComponents(); }
    void tick(float dt) { root_->updateComponents(dt); }
    void physicsTick(float dt) { root_->physicsUpdateComponents(dt); }

    // Deep copy (geometry + components; not selection). Used to snapshot the
    // scene when entering play mode so edits during play can be rolled back.
    Scene clone() const;

    // Build a small default scene so the editor is not empty on first launch.
    static Scene makeSample();

private:
    std::string name_;
    std::unique_ptr<Actor> root_;
    Actor* selected_ = nullptr;
};

} // namespace crate
