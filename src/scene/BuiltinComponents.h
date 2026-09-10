#pragma once
#include "scene/Actor.h"
#include "scene/Component.h"

#include <string>

namespace crate {

// Renders a mesh at the owning actor's world transform. The first real
// component: it either draws a built-in primitive or an imported model
// (MeshLibrary key = meshPath). The renderer walks the scene looking for it.
class MeshRenderer : public Component {
public:
    const char* typeName() const override { return "Mesh Renderer"; }
    void drawInspector() override;
    std::unique_ptr<Component> clone() const override {
        return std::make_unique<MeshRenderer>(*this);
    }

    bool usePrimitive = true;
    std::string primitive = "Cube"; // Cube/Sphere/Cylinder/Capsule/Plane/Quad
    std::string meshPath;           // imported model, used when !usePrimitive
    std::string materialRef;        // MaterialLibrary name; empty = use tint/texture
    std::string texturePath;        // albedo override when no material is set
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool castShadows = true;

    // MeshLibrary key for the geometry: empty -> use the primitive fallback.
    std::string meshKey() const { return usePrimitive ? std::string() : meshPath; }
    const std::string& primitiveName() const { return primitive; }
};

// A tiny demonstration component: spins its actor about an axis while playing.
// Proves the Start/Update lifecycle and shows up in the Add Component menu.
class SpinnerComponent : public Component {
public:
    const char* typeName() const override { return "Spinner"; }

    void start() override;
    void update(float dt) override;
    void drawInspector() override;

    std::unique_ptr<Component> clone() const override {
        return std::make_unique<SpinnerComponent>(*this);
    }

    float degreesPerSecond = 90.0f;
    int axis = 1; // 0=X 1=Y 2=Z
};

} // namespace crate
