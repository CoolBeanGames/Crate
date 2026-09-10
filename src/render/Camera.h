#pragma once
#include "core/Math.h"

namespace crate {

// Orbit camera: looks at a target point from a yaw/pitch/distance. Driven by the
// viewport (drag to orbit, wheel to zoom).
struct OrbitCamera {
    Vec3 target{0.0f, 0.5f, 0.0f};
    float yaw = 35.0f;    // degrees
    float pitch = 28.0f;  // degrees
    float distance = 12.0f;
    float fovY = 55.0f;
    float nearZ = 0.05f;
    float farZ = 500.0f;

    Vec3 eye() const {
        float cp = std::cos(radians(pitch));
        Vec3 dir{cp * std::sin(radians(yaw)), std::sin(radians(pitch)), cp * std::cos(radians(yaw))};
        return target - dir * distance;
    }

    Mat4 view() const { return Mat4::lookAtLH(eye(), target, {0, 1, 0}); }
    Mat4 proj(float aspect) const { return Mat4::perspectiveLH(fovY, aspect, nearZ, farZ); }

    void orbit(float dYaw, float dPitch) {
        yaw += dYaw;
        pitch += dPitch;
        if (pitch > 89.0f) pitch = 89.0f;
        if (pitch < -89.0f) pitch = -89.0f;
    }
    void zoom(float delta) {
        distance *= (1.0f - delta * 0.1f);
        if (distance < 0.5f) distance = 0.5f;
        if (distance > 200.0f) distance = 200.0f;
    }
};

} // namespace crate
