#pragma once
#include <cmath>

struct Vec3 {
    float x, y, z;
};

inline Vec3 v3sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 v3cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 v3norm(Vec3 a) {
    float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return {a.x / l, a.y / l, a.z / l};
}

// Tabletop orbit camera. Default framing: slightly above the table looking
// gently down at the character, ~40 degree vertical FOV (50mm-ish).
struct OrbitCamera {
    float azimuth = 0.30f;
    float elevation = 0.10f;
    float distance = 2.7f;
    Vec3 target = {0.f, 0.50f, 0.f};
    float fovY = 0.62f;

    Vec3 pos() const {
        return {target.x + distance * std::sin(azimuth) * std::cos(elevation),
                target.y + distance * std::sin(elevation),
                target.z + distance * std::cos(azimuth) * std::cos(elevation)};
    }
    void basis(Vec3& fwd, Vec3& right, Vec3& up) const {
        Vec3 p = pos();
        fwd = v3norm(v3sub(target, p));
        right = v3norm(v3cross(fwd, Vec3{0.f, 1.f, 0.f}));
        up = v3cross(right, fwd);
    }
};
