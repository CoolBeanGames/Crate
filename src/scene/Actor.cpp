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

std::unique_ptr<Actor> Actor::clone() const {
    std::unique_ptr<Actor> c(cloneSelf());
    c->transform_ = transform_;
    c->visible_ = visible_;
    c->enabled_ = enabled_;
    for (const auto& ch : children_)
        c->addChild(ch->clone());
    return c;
}

} // namespace crate
