#include "render/Mesh.h"
#include <algorithm>
#include <cctype>

namespace crate::primitives {

static void addQuad(MeshData& m, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n) {
    uint32_t base = static_cast<uint32_t>(m.vertices.size());
    m.vertices.push_back({a, n, 0, 0});
    m.vertices.push_back({b, n, 1, 0});
    m.vertices.push_back({c, n, 1, 1});
    m.vertices.push_back({d, n, 0, 1});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

MeshData cube() {
    MeshData m;
    const float h = 0.5f;
    addQuad(m, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});     // front
    addQuad(m, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1}); // back
    addQuad(m, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-1, 0, 0}); // left
    addQuad(m, {h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}, {1, 0, 0});      // right
    addQuad(m, {-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}, {0, 1, 0});      // top
    addQuad(m, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}, {0, -1, 0}); // bottom
    return m;
}

MeshData quad() {
    MeshData m;
    const float h = 0.5f;
    addQuad(m, {-h, -h, 0}, {h, -h, 0}, {h, h, 0}, {-h, h, 0}, {0, 0, 1});
    return m;
}

MeshData plane() {
    MeshData m;
    const float h = 0.5f;
    addQuad(m, {-h, 0, -h}, {-h, 0, h}, {h, 0, h}, {h, 0, -h}, {0, 1, 0});
    return m;
}

MeshData sphere(int segments, int rings) {
    MeshData m;
    for (int y = 0; y <= rings; ++y) {
        float vv = static_cast<float>(y) / rings;
        float phi = vv * kPi;
        for (int x = 0; x <= segments; ++x) {
            float uu = static_cast<float>(x) / segments;
            float theta = uu * 2.0f * kPi;
            Vec3 p{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            m.vertices.push_back({p * 0.5f, normalize(p), uu, vv});
        }
    }
    int stride = segments + 1;
    for (int y = 0; y < rings; ++y)
        for (int x = 0; x < segments; ++x) {
            uint32_t i0 = y * stride + x, i1 = i0 + 1, i2 = i0 + stride, i3 = i2 + 1;
            m.indices.insert(m.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    return m;
}

MeshData cylinder(int segments) {
    MeshData m;
    const float h = 0.5f;
    // Side wall.
    for (int x = 0; x <= segments; ++x) {
        float uu = static_cast<float>(x) / segments;
        float theta = uu * 2.0f * kPi;
        Vec3 n{std::cos(theta), 0, std::sin(theta)};
        m.vertices.push_back({{n.x * 0.5f, -h, n.z * 0.5f}, n, uu, 1});
        m.vertices.push_back({{n.x * 0.5f, h, n.z * 0.5f}, n, uu, 0});
    }
    for (int x = 0; x < segments; ++x) {
        uint32_t i = x * 2;
        m.indices.insert(m.indices.end(), {i, i + 1, i + 2, i + 2, i + 1, i + 3});
    }
    // Caps.
    auto cap = [&](float y, Vec3 n, bool flip) {
        uint32_t center = static_cast<uint32_t>(m.vertices.size());
        m.vertices.push_back({{0, y, 0}, n, 0.5f, 0.5f});
        uint32_t start = static_cast<uint32_t>(m.vertices.size());
        for (int x = 0; x <= segments; ++x) {
            float theta = static_cast<float>(x) / segments * 2.0f * kPi;
            m.vertices.push_back(
                {{std::cos(theta) * 0.5f, y, std::sin(theta) * 0.5f}, n, 0, 0});
        }
        for (int x = 0; x < segments; ++x) {
            if (flip)
                m.indices.insert(m.indices.end(), {center, start + x + 1, start + x});
            else
                m.indices.insert(m.indices.end(), {center, start + x, start + x + 1});
        }
    };
    cap(h, {0, 1, 0}, false);
    cap(-h, {0, -1, 0}, true);
    return m;
}

MeshData capsule(int segments, int rings) {
    // Cylinder body of height 1 with two hemispherical caps of radius 0.5.
    MeshData m;
    const float r = 0.5f;
    const float cyl = 0.5f; // half the straight section
    auto ring = [&](float y, float radius, float vv) {
        for (int x = 0; x <= segments; ++x) {
            float uu = static_cast<float>(x) / segments;
            float theta = uu * 2.0f * kPi;
            Vec3 pos{std::cos(theta) * radius, y, std::sin(theta) * radius};
            Vec3 n = normalize(Vec3{std::cos(theta), (y > cyl ? (y - cyl) : (y < -cyl ? (y + cyl) : 0.0f)) / r,
                                    std::sin(theta)});
            m.vertices.push_back({pos, n, uu, vv});
        }
    };
    std::vector<float> ys;
    std::vector<float> radii;
    for (int i = 0; i <= rings; ++i) {
        float a = static_cast<float>(i) / rings * (kPi * 0.5f);
        ys.push_back(cyl + std::sin(a) * r);
        radii.push_back(std::cos(a) * r);
    }
    std::reverse(ys.begin(), ys.end());
    std::reverse(radii.begin(), radii.end());
    ys.push_back(-cyl);
    radii.push_back(r);
    for (int i = 0; i <= rings; ++i) {
        float a = static_cast<float>(i) / rings * (kPi * 0.5f);
        ys.push_back(-cyl - std::sin(a) * r);
        radii.push_back(std::cos(a) * r);
    }
    for (size_t i = 0; i < ys.size(); ++i)
        ring(ys[i], radii[i], static_cast<float>(i) / (ys.size() - 1));
    int stride = segments + 1;
    for (size_t y = 0; y + 1 < ys.size(); ++y)
        for (int x = 0; x < segments; ++x) {
            uint32_t i0 = static_cast<uint32_t>(y * stride + x), i1 = i0 + 1,
                     i2 = i0 + stride, i3 = i2 + 1;
            m.indices.insert(m.indices.end(), {i0, i1, i2, i1, i3, i2});
        }
    return m;
}

MeshData byName(const std::string& name) {
    std::string n;
    for (char c : name)
        n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n == "quad") return quad();
    if (n == "plane") return plane();
    if (n == "sphere") return sphere();
    if (n == "cylinder") return cylinder();
    if (n == "capsule") return capsule();
    return cube();
}

} // namespace crate::primitives
