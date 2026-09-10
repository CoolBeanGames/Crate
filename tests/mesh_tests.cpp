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

int main() {
    int rc = 0;
    rc |= check("cube", primitives::cube());
    rc |= check("quad", primitives::quad());
    rc |= check("plane", primitives::plane());
    rc |= check("sphere", primitives::sphere());
    rc |= check("cylinder", primitives::cylinder());
    rc |= check("capsule", primitives::capsule());
    rc |= check("byName", primitives::byName("SPHERE"));
    if (rc == 0)
        std::printf("ok  all primitives valid\n");
    return rc;
}
