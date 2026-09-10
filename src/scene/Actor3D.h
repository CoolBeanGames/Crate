#pragma once
#include "scene/Actor.h"

namespace crate {

// Base for everything that lives in the 3D world. Rendering, physics, etc. are
// added as components (see MeshRenderer). Concrete 3D actor subtypes (cameras,
// lights) may still specialise this later.
class Actor3D : public Actor {
public:
    explicit Actor3D(std::string name = "Actor3D") : Actor(std::move(name)) {}
    const char* typeName() const override { return "ACTOR3D"; }

protected:
    Actor* cloneSelf() const override { return new Actor3D(name_); }
};

} // namespace crate
