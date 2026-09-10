#include "scene/Scene.h"
#include "core/Log.h"
#include "scene/Actor2D.h"
#include "scene/Actor3D.h"

namespace crate {

namespace {
int countRecursive(const Actor& a) {
    int n = 0;
    for (const auto& c : a.children())
        n += 1 + countRecursive(*c);
    return n;
}
bool descendantContains(const Actor& a, const Actor* target) {
    for (const auto& c : a.children()) {
        if (c.get() == target || descendantContains(*c, target))
            return true;
    }
    return false;
}
} // namespace

Scene::Scene(std::string name)
    : name_(std::move(name)), root_(std::make_unique<Actor>("Scene Root")) {}

Actor* Scene::add(std::unique_ptr<Actor> actor, Actor* parent, int index) {
    Actor* target = parent ? parent : root_.get();
    return target->addChild(std::move(actor), index);
}

void Scene::remove(Actor* actor) {
    if (!actor || !actor->parent())
        return;
    if (selected_ == actor || (selected_ && selected_->isDescendantOf(actor)))
        selected_ = nullptr;
    std::string name = actor->name();
    actor->parent()->removeChild(actor); // unique_ptr drops here, freeing subtree
    CR_LOG("scene", "Removed actor '" + name + "'");
}

bool Scene::contains(const Actor* actor) const {
    return actor && actor != root_.get() && descendantContains(*root_, actor);
}

bool Scene::reparent(Actor* actor, Actor* newParent, int index, bool keepWorld) {
    if (!contains(actor))
        return false;
    Actor* target = newParent ? newParent : root_.get();
    if (target != root_.get() && !contains(target))
        return false;
    if (target == actor || target->isDescendantOf(actor))
        return false; // cycle

    const Transform desiredWorld = actor->worldTransform();
    Actor* oldParent = actor->parent();
    int oldIndex = actor->indexInParent();

    // Removing before re-inserting can shift the target index within the same
    // parent; account for that so a drag "down by one" lands where expected.
    if (oldParent == target && index > oldIndex)
        --index;

    std::unique_ptr<Actor> owned = oldParent->removeChild(actor);
    if (!owned)
        return false;

    if (keepWorld) {
        const Transform parentWorld =
            (target == root_.get()) ? Transform{} : target->worldTransform();
        owned->transform() = Transform::localUnder(parentWorld, desiredWorld);
    }

    target->addChild(std::move(owned), index);
    CR_LOG("scene", "Reparented '" + actor->name() + "' under '" + target->name() + "'");
    return true;
}

Actor* Scene::duplicate(Actor* actor) {
    if (!contains(actor))
        return nullptr;
    Actor* parent = actor->parent();
    int index = actor->indexInParent();
    std::unique_ptr<Actor> copy = actor->clone();
    copy->setName(actor->name() + " Copy");
    Actor* raw = parent->addChild(std::move(copy), index + 1);
    CR_LOG("scene", "Duplicated '" + actor->name() + "'");
    return raw;
}

int Scene::actorCount() const { return countRecursive(*root_); }

Scene Scene::makeSample() {
    Scene s("Sample Scene");

    auto* set = s.add(std::make_unique<Actor3D>("-- SET --"));

    auto crate = std::make_unique<MeshActor>("Crate");
    crate->primitive = "Cube";
    crate->texturePath = "assets/uv_check.bmp";
    crate->transform().position = {0.0f, 0.75f, 0.0f};
    crate->transform().scale = {1.5f, 1.5f, 1.5f};
    s.add(std::move(crate), set);

    auto floor = std::make_unique<MeshActor>("Floor");
    floor->primitive = "Plane";
    floor->transform().scale = {6.0f, 1.0f, 6.0f};
    s.add(std::move(floor), set);

    auto lamp = std::make_unique<Actor3D>("Ceiling Lamp");
    lamp->transform().position = {0.0f, 3.0f, 0.0f};
    s.add(std::move(lamp), set);

    auto* cast = s.add(std::make_unique<Actor>("-- CAST --"));
    s.add(std::make_unique<SpriteActor>("Player Ghost"), cast);

    auto* hud = s.add(std::make_unique<Actor2D>("-- HUD --"));
    s.add(std::make_unique<UIControlActor>("Interact Prompt"), hud);

    return s;
}

} // namespace crate
