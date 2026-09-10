#include "scene/Scene.h"
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
} // namespace

Scene::Scene(std::string name)
    : name_(std::move(name)), root_(std::make_unique<Actor>("Scene Root")) {}

Actor* Scene::add(std::unique_ptr<Actor> actor, Actor* parent) {
    Actor* target = parent ? parent : root_.get();
    return target->addChild(std::move(actor));
}

void Scene::remove(Actor* actor) {
    if (!actor || !actor->parent())
        return;
    if (selected_ == actor || (selected_ && selected_->isDescendantOf(actor)))
        selected_ = nullptr;
    actor->parent()->removeChild(actor); // unique_ptr drops here, freeing subtree
}

int Scene::actorCount() const { return countRecursive(*root_); }

Scene Scene::makeSample() {
    Scene s("Sample Scene");

    auto* set = s.add(std::make_unique<Actor3D>("-- SET --"));

    auto crate = std::make_unique<MeshActor>("Crate");
    crate->primitive = "Cube";
    crate->transform().position = {0.0f, 0.5f, 0.0f};
    s.add(std::move(crate), set);

    auto floor = std::make_unique<MeshActor>("Floor");
    floor->primitive = "Plane";
    floor->transform().scale = {10.0f, 1.0f, 10.0f};
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
