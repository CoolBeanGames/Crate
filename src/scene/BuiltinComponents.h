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
    bool receiveShadows = true;

    // Primitive sizing (task 84), independent of the actor's Transform.scale:
    // a physics collider will need to size itself from these same numbers
    // later without also inheriting whatever scale the transform is used for
    // elsewhere. Only the field(s) matching `primitive`'s shape apply; the
    // renderer picks among them by name (see Renderer::drawActor).
    float boxSize[3] = {1.0f, 1.0f, 1.0f}; // Cube: full width/height/depth
    float radius = 0.5f;                    // Sphere/Cylinder/Capsule
    float height = 1.0f;                    // Cylinder/Capsule
    float planeSize[2] = {1.0f, 1.0f};      // Plane/Quad: width, depth (or height)

    // Baked contribution of static lights (task 59), filled by "Bake Lightmaps".
    // Sampled with an up-facing normal at the object's position - a coarse
    // per-object bake rather than a real per-texel lightmap.
    float bakedLight[3] = {0.0f, 0.0f, 0.0f};
    bool bakedValid = false;

    // MeshLibrary key for the geometry: empty -> use the primitive fallback.
    std::string meshKey() const { return usePrimitive ? std::string() : meshPath; }
    const std::string& primitiveName() const { return primitive; }
};

// A light source. The renderer collects every enabled LightComponent in the
// scene and feeds them to a simple per-vertex (Gourand) lighting model.
class LightComponent : public Component {
public:
    enum class Type { Directional, Point, Spot };

    const char* typeName() const override { return "Light"; }
    void drawInspector() override;
    std::unique_ptr<Component> clone() const override {
        return std::make_unique<LightComponent>(*this);
    }

    Type type = Type::Point;
    float color[3] = {1.0f, 0.94f, 0.85f};
    float intensity = 1.0f;
    float range = 8.0f;        // point / spot falloff distance
    float spotInnerDeg = 22.0f; // full brightness inside this cone half-angle
    float spotOuterDeg = 32.0f; // zero past this one

    // Static lights are excluded from the real-time pass; their contribution
    // only reaches meshes/probes via "Bake Lighting" (task 59).
    bool isStatic = false;
};

// Scene-wide height/distance fog. The renderer looks for the first enabled
// FogComponent anywhere in the scene tree and, if found, fades every pixel's
// colour toward `color` based on distance from the camera (start..end) and,
// if heightRange > 0, on how far above this actor's own world Y the pixel
// sits (full fog at/below that height, fading out over heightRange units).
// heightRange == 0 disables the height term entirely (pure distance fog).
class FogComponent : public Component {
public:
    const char* typeName() const override { return "Fog"; }
    void drawInspector() override;
    std::unique_ptr<Component> clone() const override {
        return std::make_unique<FogComponent>(*this);
    }

    float color[3] = {0.043f, 0.051f, 0.070f};
    float start = 6.0f;
    float end = 40.0f;
    float heightRange = 0.0f; // 0 = disabled (no height falloff)
};

// Global light-reactive fog: unlike Fog above (a simple distance/height
// colour fade), this fills the whole world as a translucent participating
// medium that reacts to scene lights. The renderer looks for the first
// enabled VolumetricFogComponent anywhere in the scene tree -- like Fog, its
// owning actor's transform is irrelevant, only its presence and settings
// matter.
class VolumetricFogComponent : public Component {
public:
    const char* typeName() const override { return "Volumetric Fog"; }
    void drawInspector() override;
    std::unique_ptr<Component> clone() const override {
        return std::make_unique<VolumetricFogComponent>(*this);
    }

    float color[3] = {0.6f, 0.6f, 0.65f};
    float density = 0.35f; // 0 = invisible, 1 = fully opaque
};

// A viewpoint Game View can render from (task 77). Only one CameraComponent
// in the whole scene is ever `enabled` at a time -- the editor enforces that
// exclusivity whenever one is added or toggled (see EditorApp::activateCamera
// / deactivateCamera), falling back to another present camera, if any, when
// the active one is turned off. Game View shows the free Scene View camera's
// placeholder text when no CameraComponent is enabled anywhere in the scene.
class CameraComponent : public Component {
public:
    const char* typeName() const override { return "Camera"; }
    void drawInspector() override;
    std::unique_ptr<Component> clone() const override {
        return std::make_unique<CameraComponent>(*this);
    }

    float fovY = 55.0f;
    float nearZ = 0.01f;
    float farZ = 500.0f;
};

// A fixed sample point for baked static lighting. Dynamic (moving) actors with
// no bake of their own borrow the nearest probe's baked colour as extra
// ambient each frame, so they still read as lit by static-only lights.
class LightProbeComponent : public Component {
public:
    const char* typeName() const override { return "Light Probe"; }
    void drawInspector() override;
    std::unique_ptr<Component> clone() const override {
        return std::make_unique<LightProbeComponent>(*this);
    }

    float bakedLight[3] = {0.0f, 0.0f, 0.0f};
    bool bakedValid = false;
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
