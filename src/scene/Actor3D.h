#pragma once
#include "scene/Actor.h"

namespace crate {

// Base for everything that lives in the 3D world. Concrete 3D types (meshes,
// lights, cameras) derive from this.
class Actor3D : public Actor {
public:
    explicit Actor3D(std::string name = "Actor3D") : Actor(std::move(name)) {}
    const char* typeName() const override { return "ACTOR3D"; }

protected:
    Actor* cloneSelf() const override { return new Actor3D(name_); }
};

// A renderable 3D mesh. The mesh itself is not loaded yet (rendering branch);
// for now it just carries a source path and a primitive fallback so the
// hierarchy and inspector have something concrete to show.
class MeshActor : public Actor3D {
public:
    explicit MeshActor(std::string name = "Mesh") : Actor3D(std::move(name)) {}
    const char* typeName() const override { return "MESH"; }

    std::string meshPath;           // e.g. "assets/props/crate.obj"
    std::string primitive = "Cube"; // used when meshPath is empty
    bool castShadows = true;

protected:
    Actor* cloneSelf() const override {
        auto* m = new MeshActor(name_);
        m->meshPath = meshPath;
        m->primitive = primitive;
        m->castShadows = castShadows;
        return m;
    }
};

} // namespace crate
