// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Small vector and matrix maths for the GPU scenes (Phase 4a design §5.1,
// §6): column-major matrices (m[c*4 + r]), a right-handed world, clip depth
// 0..1 (the D3D convention; backends adapt, §5.3). Header-only, no Qt.
//
// Only + - * / and sqrt are used where a result is hashed or stored as bytes
// (the noise below), so every compiler gives the same bits (-ffp-contract=off).
// Trigonometry runs in double.

#include <cmath>
#include <cstdint>

namespace sb::gpu {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
};
struct Mat4 {
    float m[16] = {};
    static Mat4 identity()
    {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
        return r;
    }
};

inline Mat4 operator*(const Mat4 &a, const Mat4 &b)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 normalize(Vec3 v)
{
    const float len = std::sqrt(dot(v, v));
    return len > 0.f ? Vec3{v.x / len, v.y / len, v.z / len} : Vec3{0, 0, 0};
}

// RH, depth 0..1 (glm perspectiveRH_ZO).
inline Mat4 perspective(float fovYRadians, float aspect, float zNear, float zFar)
{
    const float t = float(std::tan(double(fovYRadians) / 2.0));
    Mat4 r;
    r.m[0] = 1.f / (aspect * t);
    r.m[5] = 1.f / t;
    r.m[10] = zFar / (zNear - zFar);
    r.m[11] = -1.f;
    r.m[14] = -(zFar * zNear) / (zFar - zNear);
    return r;
}

// RH (glm lookAtRH).
inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up)
{
    const Vec3 f = normalize(target - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);
    Mat4 r = Mat4::identity();
    r.m[0] = s.x; r.m[1] = u.x; r.m[2] = -f.x;
    r.m[4] = s.y; r.m[5] = u.y; r.m[6] = -f.y;
    r.m[8] = s.z; r.m[9] = u.z; r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
}

inline Mat4 translate(Vec3 t)
{
    Mat4 r = Mat4::identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}
inline Mat4 scale(Vec3 s)
{
    Mat4 r = Mat4::identity();
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    return r;
}
// Rodrigues; `axis` need not be normalised.
inline Mat4 rotate(float radians, Vec3 axis)
{
    const Vec3 a = normalize(axis);
    const float c = float(std::cos(double(radians))), s = float(std::sin(double(radians))), k = 1.f - c;
    Mat4 r = Mat4::identity();
    r.m[0] = c + a.x * a.x * k;
    r.m[1] = a.y * a.x * k + a.z * s;
    r.m[2] = a.z * a.x * k - a.y * s;
    r.m[4] = a.x * a.y * k - a.z * s;
    r.m[5] = c + a.y * a.y * k;
    r.m[6] = a.z * a.y * k + a.x * s;
    r.m[8] = a.x * a.z * k + a.y * s;
    r.m[9] = a.y * a.z * k - a.x * s;
    r.m[10] = c + a.z * a.z * k;
    return r;
}
// RH, depth 0..1 (glm orthoRH_ZO).
inline Mat4 orthographic(float l, float r, float b, float t, float n, float f)
{
    Mat4 m = Mat4::identity();
    m.m[0] = 2.f / (r - l);
    m.m[5] = 2.f / (t - b);
    m.m[10] = -1.f / (f - n);
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    m.m[14] = -n / (f - n);
    return m;
}

// h in [0,1); s, v in [0,1].
inline Vec3 hsv(float h, float s, float v)
{
    h = h - std::floor(h);
    const float h6 = h * 6.f;
    const int i = int(h6);
    const float f = h6 - float(i);
    const float p = v * (1.f - s), q = v * (1.f - s * f), t = v * (1.f - s * (1.f - f));
    switch (i % 6) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
    }
}

// --- value noise (integer lattice hash, quintic interpolation) ---------------

inline std::uint32_t hash3(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t seed)
{
    std::uint32_t h = seed ^ 0x9E3779B9u;
    h ^= std::uint32_t(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h *= 0xC2B2AE35u;
    h ^= std::uint32_t(y) * 0x27D4EB2Fu;
    h = (h << 11) | (h >> 21);
    h *= 0x165667B1u;
    h ^= std::uint32_t(z) * 0x9E3779B1u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
// [-1, 1]
inline float lattice(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t seed)
{
    return float(hash3(x, y, z, seed) >> 8) * (2.f / 16777216.f) - 1.f;
}
inline float quintic(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// 3D value noise in [-1, 1].
inline float valueNoise(float x, float y, float z, std::uint32_t seed)
{
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const std::int32_t ix = std::int32_t(fx), iy = std::int32_t(fy), iz = std::int32_t(fz);
    const float u = quintic(x - fx), v = quintic(y - fy), w = quintic(z - fz);
    const float c000 = lattice(ix, iy, iz, seed), c100 = lattice(ix + 1, iy, iz, seed);
    const float c010 = lattice(ix, iy + 1, iz, seed), c110 = lattice(ix + 1, iy + 1, iz, seed);
    const float c001 = lattice(ix, iy, iz + 1, seed), c101 = lattice(ix + 1, iy, iz + 1, seed);
    const float c011 = lattice(ix, iy + 1, iz + 1, seed), c111 = lattice(ix + 1, iy + 1, iz + 1, seed);
    const float x00 = lerpf(c000, c100, u), x10 = lerpf(c010, c110, u);
    const float x01 = lerpf(c001, c101, u), x11 = lerpf(c011, c111, u);
    return lerpf(lerpf(x00, x10, v), lerpf(x01, x11, v), w);
}
// `octaves` of value noise, each at twice the frequency and half the weight,
// normalised so the result stays in [-1, 1].
inline float fbm(float x, float y, float z, std::uint32_t seed, int octaves)
{
    float sum = 0.f, amp = 1.f, norm = 0.f, f = 1.f;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * valueNoise(x * f, y * f, z * f, seed + std::uint32_t(o) * 0x1000193u);
        norm += amp;
        amp *= 0.5f;
        f *= 2.f;
    }
    return sum / norm;
}

} // namespace sb::gpu
