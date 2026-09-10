#pragma once
#include "core/Math.h"
#include <cstdint>
#include <string>
#include <vector>

namespace crate {

struct Vertex {
    Vec3 position;
    Vec3 normal;
    float u = 0.0f, v = 0.0f;
};

// CPU-side mesh data. GPU upload lives in the renderer.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// Built-in primitives. Names match what MeshRenderer / MeshActor store.
namespace primitives {
MeshData cube();
MeshData quad();     // 1x1 facing +Z
MeshData plane();    // 1x1 ground plane, facing +Y
MeshData sphere(int segments = 24, int rings = 16);
MeshData cylinder(int segments = 24);
MeshData capsule(int segments = 24, int rings = 8);

// Case-insensitive lookup; returns cube for unknown names.
MeshData byName(const std::string& name);
} // namespace primitives

} // namespace crate
