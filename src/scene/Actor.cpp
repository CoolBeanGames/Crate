#include "scene/Actor.h"

#include "assets/AssetDatabase.h"

#include <algorithm>

namespace crate {

uint64_t Actor::nextId_ = 1;

Actor::Actor(std::string name)
    : id_(nextId_++), auid_(AssetDatabase::newUuid()), name_(std::move(name)) {}

Transform Actor::worldTransform() const {
    if (!parent_)
        return transform_;
    return transform_.composedWith(parent_->worldTransform());
}

Actor* Actor::addChild(std::unique_ptr<Actor> child, int index) {
    if (!child)
        return nullptr;
    child->parent_ = this;
    Actor* raw = child.get();
    if (index < 0 || index >= static_cast<int>(children_.size()))
        children_.push_back(std::move(child));
    else
        children_.insert(children_.begin() + index, std::move(child));
    return raw;
}

std::unique_ptr<Actor> Actor::removeChild(Actor* child) {
    auto it = std::find_if(children_.begin(), children_.end(),
                           [child](const std::unique_ptr<Actor>& c) { return c.get() == child; });
    if (it == children_.end())
        return nullptr;
    std::unique_ptr<Actor> owned = std::move(*it);
    children_.erase(it);
    owned->parent_ = nullptr;
    return owned;
}

void Actor::reparent(Actor* newParent) {
    if (!parent_ || !newParent || newParent == parent_)
        return;
    if (newParent == this || newParent->isDescendantOf(this))
        return; // would create a cycle
    std::unique_ptr<Actor> self = parent_->removeChild(this);
    if (self)
        newParent->addChild(std::move(self));
}

int Actor::indexInParent() const {
    if (!parent_)
        return -1;
    const auto& sibs = parent_->children_;
    for (size_t i = 0; i < sibs.size(); ++i)
        if (sibs[i].get() == this)
            return static_cast<int>(i);
    return -1;
}

bool Actor::isDescendantOf(const Actor* other) const {
    for (const Actor* p = parent_; p; p = p->parent_)
        if (p == other)
            return true;
    return false;
}

std::unique_ptr<Actor> Actor::clone(bool preserveAuid) const {
    std::unique_ptr<Actor> c(cloneSelf());
    c->transform_ = transform_;
    c->visible_ = visible_;
    c->enabled_ = enabled_;
    if (preserveAuid)
        c->auid_ = auid_;
    for (const auto& comp : components_)
        c->addComponent(comp->clone());
    for (const auto& ch : children_)
        c->addChild(ch->clone(preserveAuid));
    return c;
}

Component* Actor::addComponent(std::unique_ptr<Component> c) {
    if (!c)
        return nullptr;
    c->actor_ = this;
    Component* raw = c.get();
    components_.push_back(std::move(c));
    return raw;
}

void Actor::removeComponent(Component* c) {
    for (auto it = components_.begin(); it != components_.end(); ++it)
        if (it->get() == c) {
            components_.erase(it);
            return;
        }
}

void Actor::startComponents() {
    for (const auto& c : components_)
        if (c->enabled)
            c->start();
    for (const auto& ch : children_)
        ch->startComponents();
}

void Actor::updateComponents(float dt) {
    for (const auto& c : components_)
        if (c->enabled)
            c->update(dt);
    for (const auto& ch : children_)
        ch->updateComponents(dt);
}

void Actor::physicsUpdateComponents(float dt) {
    for (const auto& c : components_)
        if (c->enabled)
            c->physicsUpdate(dt);
    for (const auto& ch : children_)
        ch->physicsUpdateComponents(dt);
}

} // namespace crate
