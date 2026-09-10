#include "scene/Actor.h"
#include <algorithm>

namespace crate {

uint64_t Actor::nextId_ = 1;

Actor::Actor(std::string name) : id_(nextId_++), name_(std::move(name)) {}

Transform Actor::worldTransform() const {
    if (!parent_)
        return transform_;
    return transform_.composedWith(parent_->worldTransform());
}

Actor* Actor::addChild(std::unique_ptr<Actor> child) {
    if (!child)
        return nullptr;
    child->parent_ = this;
    Actor* raw = child.get();
    children_.push_back(std::move(child));
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

bool Actor::isDescendantOf(const Actor* other) const {
    for (const Actor* p = parent_; p; p = p->parent_)
        if (p == other)
            return true;
    return false;
}

} // namespace crate
