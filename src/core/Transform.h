#pragma once
#include "core/Math.h"

namespace crate {

// A local transform relative to a parent. Rotation is stored as Euler angles in
// degrees (X, Y, Z) which is what the inspector edits directly. World-space
// values are resolved by composing with the parent chain (see Actor).
struct Transform {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotationEuler{0.0f, 0.0f, 0.0f}; // degrees
    Vec3 scale{1.0f, 1.0f, 1.0f};

    // Compose this local transform onto a parent world transform. This is a
    // simplified TRS composition: it does not apply the parent's rotation to the
    // child offset yet, which is enough for hierarchy bookkeeping and the
    // inspector. Full matrix composition arrives with the rendering branch.
    Transform composedWith(const Transform& parentWorld) const {
        Transform out;
        out.position = parentWorld.position + position * parentWorld.scale;
        out.rotationEuler = parentWorld.rotationEuler + rotationEuler;
        out.scale = parentWorld.scale * scale;
        return out;
    }

    // Inverse of composedWith: given a parent's world transform and a desired
    // world transform, return the local transform that produces it. Used when
    // reparenting so an actor keeps its place in the world.
    static Transform localUnder(const Transform& parentWorld, const Transform& desiredWorld) {
        auto safe = [](float v) { return v == 0.0f ? 1.0f : v; };
        Transform out;
        out.position = {(desiredWorld.position.x - parentWorld.position.x) / safe(parentWorld.scale.x),
                        (desiredWorld.position.y - parentWorld.position.y) / safe(parentWorld.scale.y),
                        (desiredWorld.position.z - parentWorld.position.z) / safe(parentWorld.scale.z)};
        out.rotationEuler = desiredWorld.rotationEuler - parentWorld.rotationEuler;
        out.scale = {desiredWorld.scale.x / safe(parentWorld.scale.x),
                     desiredWorld.scale.y / safe(parentWorld.scale.y),
                     desiredWorld.scale.z / safe(parentWorld.scale.z)};
        return out;
    }
};

} // namespace crate
