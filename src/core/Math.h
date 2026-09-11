#pragma once
#include <cmath>

// Minimal math used by the engine core. Small on purpose; a fuller library can
// replace this later without touching call sites.
namespace crate {

constexpr float kPi = 3.14159265358979323846f;
inline float radians(float deg) { return deg * (kPi / 180.0f); }

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit Vec3(float s) : x(s), y(s), z(s) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }

    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    float l = length(v);
    return l > 1e-8f ? v * (1.0f / l) : Vec3{0, 0, 0};
}

// Row-major 4x4 matrix. Vectors are treated as rows: v' = v * M. Column-major
// consumers (HLSL default) should upload Mat4::transposed().
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    float& at(int r, int c) { return m[r * 4 + c]; }
    float at(int r, int c) const { return m[r * 4 + c]; }

    static Mat4 identity() { return {}; }

    static Mat4 translation(const Vec3& t) {
        Mat4 r;
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }
    static Mat4 scale(const Vec3& s) {
        Mat4 r;
        r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
        return r;
    }
    static Mat4 rotationX(float deg) {
        float c = std::cos(radians(deg)), s = std::sin(radians(deg));
        Mat4 r;
        r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
        return r;
    }
    static Mat4 rotationY(float deg) {
        float c = std::cos(radians(deg)), s = std::sin(radians(deg));
        Mat4 r;
        r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
        return r;
    }
    static Mat4 rotationZ(float deg) {
        float c = std::cos(radians(deg)), s = std::sin(radians(deg));
        Mat4 r;
        r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
        return r;
    }
    // Euler order X * Y * Z for row vectors (v' = v * Rx * Ry * Rz). Matches
    // the order the viewport gizmo (ImGuizmo) decomposes/recomposes with.
    static Mat4 rotationEuler(const Vec3& deg) {
        return rotationX(deg.x) * rotationY(deg.y) * rotationZ(deg.z);
    }

    static Mat4 perspectiveLH(float fovYdeg, float aspect, float zn, float zf) {
        float f = 1.0f / std::tan(radians(fovYdeg) * 0.5f);
        Mat4 r{};
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = zf / (zf - zn);
        r.m[11] = 1.0f;
        r.m[14] = -zn * zf / (zf - zn);
        r.m[15] = 0.0f;
        return r;
    }

    static Mat4 orthoLH(float w, float h, float zn, float zf) {
        Mat4 r{};
        r.m[0] = 2.0f / w;
        r.m[5] = 2.0f / h;
        r.m[10] = 1.0f / (zf - zn);
        r.m[14] = -zn / (zf - zn);
        r.m[15] = 1.0f;
        return r;
    }

    static Mat4 lookAtLH(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 z = normalize(target - eye);
        Vec3 x = normalize(cross(up, z));
        Vec3 y = cross(z, x);
        Mat4 r;
        r.m[0] = x.x; r.m[1] = y.x; r.m[2] = z.x; r.m[3] = 0;
        r.m[4] = x.y; r.m[5] = y.y; r.m[6] = z.y; r.m[7] = 0;
        r.m[8] = x.z; r.m[9] = y.z; r.m[10] = z.z; r.m[11] = 0;
        r.m[12] = -dot(x, eye); r.m[13] = -dot(y, eye); r.m[14] = -dot(z, eye); r.m[15] = 1;
        return r;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k)
                    s += at(i, k) * b.at(k, j);
                r.at(i, j) = s;
            }
        return r;
    }

    Mat4 transposed() const {
        Mat4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.at(i, j) = at(j, i);
        return r;
    }
};

// Rotates/scales a direction by the upper-left 3x3 of `m` (row-vector
// convention: v' = v * M), ignoring translation. Used for derived
// transform.forward / right / up vectors.
inline Vec3 transformDirection(const Vec3& v, const Mat4& m) {
    return {
        v.x * m.at(0, 0) + v.y * m.at(1, 0) + v.z * m.at(2, 0),
        v.x * m.at(0, 1) + v.y * m.at(1, 1) + v.z * m.at(2, 1),
        v.x * m.at(0, 2) + v.y * m.at(1, 2) + v.z * m.at(2, 2),
    };
}

} // namespace crate
