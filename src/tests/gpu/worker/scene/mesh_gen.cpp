// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/scene/mesh_gen.h"

#include <cmath>
#include <unordered_map>
#include <utility>

namespace sb::gpu {

namespace {

constexpr double kPi = 3.14159265358979323846;

Vec3 pos(const Vertex &v) { return {v.px, v.py, v.pz}; }
Vec3 nrm(const Vertex &v) { return {v.nx, v.ny, v.nz}; }

// Adds vertices and triangles; a triangle is flipped when its winding
// disagrees with the vertices' own normals (counter-clockwise = facing along them).
struct Builder {
    Mesh m;
    std::uint32_t vertex(Vec3 p, Vec3 n, float u, float v)
    {
        m.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, u, v});
        return std::uint32_t(m.vertices.size() - 1);
    }
    void tri(std::uint32_t a, std::uint32_t b, std::uint32_t c)
    {
        const Vertex &va = m.vertices[a], &vb = m.vertices[b], &vc = m.vertices[c];
        const Vec3 face = cross(pos(vb) - pos(va), pos(vc) - pos(va));
        const Vec3 sum = nrm(va) + nrm(vb) + nrm(vc);
        if (dot(face, sum) < 0.f)
            std::swap(b, c);
        m.indices.push_back(a);
        m.indices.push_back(b);
        m.indices.push_back(c);
    }
    void quad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d)  // a b / c d : a-b-c, c-b-d
    {
        tri(a, b, c);
        tri(c, b, d);
    }
};

// A cellsU x cellsV grid in the plane through `centre` spanned by axisU and
// axisV (sizes su, sv), facing along `normal`, UVs tiled `tile` times.
void addPlane(Builder &b, Vec3 centre, Vec3 axisU, Vec3 axisV, int cu, int cv, float su, float sv, Vec3 normal, float tile)
{
    const std::uint32_t first = std::uint32_t(b.m.vertices.size());
    for (int j = 0; j <= cv; ++j) {
        for (int i = 0; i <= cu; ++i) {
            const float fu = float(i) / float(cu), fv = float(j) / float(cv);
            const Vec3 p = centre + axisU * ((fu - 0.5f) * su) + axisV * ((fv - 0.5f) * sv);
            b.vertex(p, normal, fu * tile, fv * tile);
        }
    }
    const std::uint32_t row = std::uint32_t(cu + 1);
    for (int j = 0; j < cv; ++j) {
        for (int i = 0; i < cu; ++i) {
            const std::uint32_t a = first + std::uint32_t(j) * row + std::uint32_t(i);
            b.quad(a, a + 1, a + row, a + row + 1);
        }
    }
}

void recomputeNormals(Mesh &m)
{
    std::vector<Vec3> acc(m.vertices.size());
    for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const Vertex &a = m.vertices[m.indices[i]], &b = m.vertices[m.indices[i + 1]], &c = m.vertices[m.indices[i + 2]];
        const Vec3 face = cross(pos(b) - pos(a), pos(c) - pos(a));  // length = twice the area: area-weighted
        for (int k = 0; k < 3; ++k)
            acc[m.indices[i + std::size_t(k)]] = acc[m.indices[i + std::size_t(k)]] + face;
    }
    for (std::size_t i = 0; i < m.vertices.size(); ++i) {
        const Vec3 n = normalize(acc[i]);
        m.vertices[i].nx = n.x;
        m.vertices[i].ny = n.y;
        m.vertices[i].nz = n.z;
    }
}

} // namespace

Mesh icosphere(int levels, float radius)
{
    Mesh m;
    const float t = (1.f + std::sqrt(5.f)) / 2.f;
    const Vec3 base[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t},  {0, 1, t},
                           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    const std::uint32_t faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                        {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
                                        {3, 8, 9},  {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    std::vector<Vec3> dirs;
    for (const Vec3 &p : base)
        dirs.push_back(normalize(p));
    std::vector<std::uint32_t> tris;
    for (const auto &f : faces)
        tris.insert(tris.end(), {f[0], f[1], f[2]});

    for (int level = 0; level < levels; ++level) {
        std::unordered_map<std::uint64_t, std::uint32_t> mid;
        const auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const std::uint64_t key = a < b ? (std::uint64_t(a) << 32) | b : (std::uint64_t(b) << 32) | a;
            const auto it = mid.find(key);
            if (it != mid.end())
                return it->second;
            dirs.push_back(normalize((dirs[a] + dirs[b]) * 0.5f));
            const auto index = std::uint32_t(dirs.size() - 1);
            mid.emplace(key, index);
            return index;
        };
        std::vector<std::uint32_t> next;
        next.reserve(tris.size() * 4);
        for (std::size_t i = 0; i < tris.size(); i += 3) {
            const std::uint32_t a = tris[i], b = tris[i + 1], c = tris[i + 2];
            const std::uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        tris.swap(next);
    }

    m.vertices.reserve(dirs.size());
    for (const Vec3 &d : dirs) {
        const float u = float(std::atan2(double(d.z), double(d.x)) / (2.0 * kPi) + 0.5);
        const float v = float(std::acos(double(d.y)) / kPi);
        m.vertices.push_back({d.x * radius, d.y * radius, d.z * radius, d.x, d.y, d.z, u, v});
    }
    m.indices = std::move(tris);
    return m;
}

Mesh displaced(Mesh m, std::uint32_t seed, float amplitude, float frequency)
{
    for (Vertex &v : m.vertices) {
        const Vec3 dir = normalize(pos(v));
        const float r = 1.f + amplitude * fbm(dir.x * frequency, dir.y * frequency, dir.z * frequency, seed, 3);
        v.px = dir.x * r;
        v.py = dir.y * r;
        v.pz = dir.z * r;
    }
    recomputeNormals(m);
    return m;
}

Mesh cube()
{
    Builder b;
    const Vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3 &normal : n) {
        const Vec3 up = std::fabs(normal.y) > 0.5f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        const Vec3 u = normalize(cross(up, normal)), v = cross(normal, u);
        const auto corner = [&](float su, float sv, float tu, float tv) {
            return b.vertex(normal * 0.5f + u * (su * 0.5f) + v * (sv * 0.5f), normal, tu, tv);
        };
        const std::uint32_t a = corner(-1, -1, 0, 0), bb = corner(1, -1, 1, 0), c = corner(-1, 1, 0, 1), d = corner(1, 1, 1, 1);
        b.quad(a, bb, c, d);
    }
    return b.m;
}

Mesh octahedron()
{
    Builder b;
    const float s = 1.f;
    for (int i = 0; i < 8; ++i) {
        const float sx = (i & 1) ? -s : s, sy = (i & 2) ? -s : s, sz = (i & 4) ? -s : s;
        const Vec3 px{sx, 0, 0}, py{0, sy, 0}, pz{0, 0, sz};
        const Vec3 n = normalize(Vec3{sx, sy, sz});
        const std::uint32_t a = b.vertex(px, n, 0, 0), bb = b.vertex(py, n, 1, 0), c = b.vertex(pz, n, 0, 1);
        b.tri(a, bb, c);
    }
    return b.m;
}

Mesh cylinder(int segments)
{
    Builder b;
    const auto ring = [&](int i) {
        const double a = 2.0 * kPi * double(i) / double(segments);
        return Vec3{float(std::cos(a)), 0.f, float(std::sin(a))};
    };
    const std::uint32_t side = std::uint32_t(b.m.vertices.size());
    for (int i = 0; i <= segments; ++i) {
        const Vec3 r = ring(i);
        const float u = float(i) / float(segments);
        b.vertex({r.x, -1.f, r.z}, r, u, 0);
        b.vertex({r.x, 1.f, r.z}, r, u, 1);
    }
    for (int i = 0; i < segments; ++i) {
        const std::uint32_t a = side + std::uint32_t(i) * 2;
        b.quad(a, a + 2, a + 1, a + 3);
    }
    for (const float y : {1.f, -1.f}) {
        const Vec3 n{0, y, 0};
        const std::uint32_t centre = b.vertex({0, y, 0}, n, 0.5f, 0.5f);
        const std::uint32_t first = std::uint32_t(b.m.vertices.size());
        for (int i = 0; i <= segments; ++i) {
            const Vec3 r = ring(i);
            b.vertex({r.x, y, r.z}, n, 0.5f + r.x * 0.5f, 0.5f + r.z * 0.5f);
        }
        for (int i = 0; i < segments; ++i)
            b.tri(centre, first + std::uint32_t(i), first + std::uint32_t(i) + 1);
    }
    return b.m;
}

Mesh cone(int segments)
{
    Builder b;
    const auto ring = [&](int i) {
        const double a = 2.0 * kPi * double(i) / double(segments);
        return Vec3{float(std::cos(a)), 0.f, float(std::sin(a))};
    };
    for (int i = 0; i < segments; ++i) {
        const Vec3 r0 = ring(i), r1 = ring(i + 1);
        const Vec3 mid = normalize(r0 + r1);
        const Vec3 n = normalize(Vec3{mid.x, 0.5f, mid.z});
        const std::uint32_t a = b.vertex({r0.x, -1.f, r0.z}, n, float(i) / float(segments), 0);
        const std::uint32_t c = b.vertex({r1.x, -1.f, r1.z}, n, float(i + 1) / float(segments), 0);
        const std::uint32_t apex = b.vertex({0, 1, 0}, n, (float(i) + 0.5f) / float(segments), 1);
        b.tri(a, c, apex);
    }
    const Vec3 n{0, -1, 0};
    const std::uint32_t centre = b.vertex({0, -1, 0}, n, 0.5f, 0.5f);
    const std::uint32_t first = std::uint32_t(b.m.vertices.size());
    for (int i = 0; i <= segments; ++i) {
        const Vec3 r = ring(i);
        b.vertex({r.x, -1.f, r.z}, n, 0.5f + r.x * 0.5f, 0.5f + r.z * 0.5f);
    }
    for (int i = 0; i < segments; ++i)
        b.tri(centre, first + std::uint32_t(i), first + std::uint32_t(i) + 1);
    return b.m;
}

Mesh torus(int major, int minor, float R, float r)
{
    Builder b;
    for (int j = 0; j <= major; ++j) {
        const double th = 2.0 * kPi * double(j) / double(major);
        const float ct = float(std::cos(th)), st = float(std::sin(th));
        for (int i = 0; i <= minor; ++i) {
            const double ph = 2.0 * kPi * double(i) / double(minor);
            const float cp = float(std::cos(ph)), sp = float(std::sin(ph));
            b.vertex({(R + r * cp) * ct, r * sp, (R + r * cp) * st}, {cp * ct, sp, cp * st}, float(j) / float(major),
                     float(i) / float(minor));
        }
    }
    const std::uint32_t row = std::uint32_t(minor + 1);
    for (int j = 0; j < major; ++j) {
        for (int i = 0; i < minor; ++i) {
            const std::uint32_t a = std::uint32_t(j) * row + std::uint32_t(i);
            b.quad(a, a + 1, a + row, a + row + 1);
        }
    }
    return b.m;
}

Mesh uvSphere(int slices, int stacks)
{
    Builder b;
    for (int j = 0; j <= stacks; ++j) {
        const double ph = kPi * double(j) / double(stacks);
        const float cp = float(std::cos(ph)), sp = float(std::sin(ph));
        for (int i = 0; i <= slices; ++i) {
            const double th = 2.0 * kPi * double(i) / double(slices);
            const Vec3 n{sp * float(std::cos(th)), cp, sp * float(std::sin(th))};
            b.vertex(n, n, float(i) / float(slices), float(j) / float(stacks));
        }
    }
    const std::uint32_t row = std::uint32_t(slices + 1);
    for (int j = 0; j < stacks; ++j) {
        for (int i = 0; i < slices; ++i) {
            const std::uint32_t a = std::uint32_t(j) * row + std::uint32_t(i), c = a + 1, bb = a + row, d = a + row + 1;
            if (j == 0)
                b.tri(a, bb, d);  // a and c are the pole
            else if (j == stacks - 1)
                b.tri(a, bb, c);  // bb and d are the pole
            else
                b.quad(a, bb, c, d);
        }
    }
    return b.m;
}

Mesh grid(int cellsX, int cellsZ, float sizeX, float sizeZ)
{
    Builder b;
    addPlane(b, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, cellsX, cellsZ, sizeX, sizeZ, {0, 1, 0}, 1.f);
    return b.m;
}

Mesh room()
{
    Builder b;
    const float tile = 8.f;
    addPlane(b, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, 64, 64, 40, 40, {0, 1, 0}, tile);      // floor
    // No ceiling (Dave, 2026-10-21): the sun reaches into the room through the open top and the
    // objects cast shadows; a closed room left the whole shadow map filled by the ceiling.
    addPlane(b, {-20, 6, 0}, {0, 0, 1}, {0, 1, 0}, 16, 16, 40, 12, {1, 0, 0}, tile);    // walls, facing in
    addPlane(b, {20, 6, 0}, {0, 0, 1}, {0, 1, 0}, 16, 16, 40, 12, {-1, 0, 0}, tile);
    addPlane(b, {0, 6, -20}, {1, 0, 0}, {0, 1, 0}, 16, 16, 40, 12, {0, 0, 1}, tile);
    addPlane(b, {0, 6, 20}, {1, 0, 0}, {0, 1, 0}, 16, 16, 40, 12, {0, 0, -1}, tile);
    return b.m;
}

std::uint64_t triangleCount(const Mesh &m) { return m.indices.size() / 3; }

} // namespace sb::gpu
