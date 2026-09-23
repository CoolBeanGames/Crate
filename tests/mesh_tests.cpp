// Primitive mesh generators: every shape must produce non-degenerate,
// triangulated geometry with finite positions and unit-ish normals.

#include "render/Mesh.h"

#include <cmath>
#include <cstdio>

using namespace crate;

static int check(const char* name, const MeshData& m) {
    if (m.vertices.size() < 3 || m.indices.empty() || m.indices.size() % 3 != 0) {
        std::printf("FAIL %s: verts=%zu indices=%zu\n", name, m.vertices.size(), m.indices.size());
        return 1;
    }
    for (uint32_t i : m.indices)
        if (i >= m.vertices.size()) {
            std::printf("FAIL %s: index %u out of range\n", name, i);
            return 1;
        }
    for (const auto& v : m.vertices) {
        if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) ||
            !std::isfinite(v.position.z)) {
            std::printf("FAIL %s: non-finite position\n", name);
            return 1;
        }
        float nl = length(v.normal);
        if (nl > 0.01f && std::fabs(nl - 1.0f) > 0.1f) {
            std::printf("FAIL %s: normal length %f\n", name, nl);
            return 1;
        }
    }
    std::printf("  %-9s verts=%3zu tris=%3zu\n", name, m.vertices.size(), m.indices.size() / 3);
    return 0;
}

// task 140: subdivision must be a real geometry change (more verts/tris),
// n=1 must reproduce the exact old single-quad plane() output (same corner
// positions, in some order), and byName()'s "Plane#N" cache-key encoding
// must parse back to the same subdivision count.
static int checkCount(const char* name, size_t got, size_t want) {
    if (got != want) {
        std::printf("FAIL %s: got %zu, want %zu\n", name, got, want);
        return 1;
    }
    return 0;
}

static bool hasCorner(const MeshData& m, Vec3 p) {
    for (const auto& v : m.vertices)
        if (length(v.position - p) < 1e-4f)
            return true;
    return false;
}

static int checkPlaneSubdivision() {
    int rc = 0;
    MeshData n1 = primitives::plane(1);
    rc |= check("plane_n1", n1);
    rc |= checkCount("plane_n1 verts", n1.vertices.size(), 4);
    rc |= checkCount("plane_n1 tris", n1.indices.size() / 3, 2);
    const float h = 0.5f;
    for (Vec3 corner : {Vec3{-h, 0, -h}, Vec3{-h, 0, h}, Vec3{h, 0, h}, Vec3{h, 0, -h}})
        if (!hasCorner(n1, corner)) {
            std::printf("FAIL plane_n1: missing corner (%.2f,%.2f,%.2f)\n", corner.x, corner.y,
                       corner.z);
            rc = 1;
        }

    MeshData n4 = primitives::plane(4);
    rc |= check("plane_n4", n4);
    rc |= checkCount("plane_n4 verts", n4.vertices.size(), 25);   // (4+1)^2
    rc |= checkCount("plane_n4 tris", n4.indices.size() / 3, 32); // 4*4*2

    // plane(0) / plane(-3) must clamp to at least 1 subdivision, not crash
    // or produce degenerate/empty geometry.
    rc |= checkCount("plane_n0 clamps", primitives::plane(0).vertices.size(), 4);
    rc |= checkCount("plane_neg clamps", primitives::plane(-3).vertices.size(), 4);

    // byName()'s "Plane#N" cache-key encoding round-trips to the right count.
    rc |= checkCount("byName Plane#1", primitives::byName("Plane#1").vertices.size(), 4);
    rc |= checkCount("byName Plane#4", primitives::byName("Plane#4").vertices.size(), 25);
    rc |= checkCount("byName Plane (no #)", primitives::byName("Plane").vertices.size(), 4);
    if (rc == 0)
        std::printf("ok  plane subdivision (task 140)\n");
    return rc;
}

int main() {
    int rc = 0;
    rc |= check("cube", primitives::cube());
    rc |= check("quad", primitives::quad());
    rc |= check("plane", primitives::plane());
    rc |= check("sphere", primitives::sphere());
    rc |= check("cylinder", primitives::cylinder());
    rc |= check("capsule", primitives::capsule());
    rc |= check("byName", primitives::byName("SPHERE"));
    rc |= checkPlaneSubdivision();
    if (rc == 0)
        std::printf("ok  all primitives valid\n");
    return rc;
}
