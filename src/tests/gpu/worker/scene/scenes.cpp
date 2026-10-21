// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/scene/scenes.h"

#include "tests/gpu/worker/scene/mesh_gen.h"
#include "tests/gpu/worker/scene/prng.h"
#include "tests/gpu/worker/scene/textures.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sb::gpu {

namespace {

constexpr double kPi = 3.14159265358979323846;

// One draw per statement (the prng.h rule).
float uniform(Xoshiro256ss &rng, float lo, float hi)
{
    const std::uint64_t r = rng.next();
    return lo + (hi - lo) * (float(r >> 40) / 16777216.f);
}

std::uint32_t scaled(std::uint32_t n, double contentScale)
{
    const double s = contentScale > 0.0 ? contentScale : 1.0;
    return std::max<std::uint32_t>(1, std::uint32_t(std::ceil(double(n) / s)));
}

Instance instance(Mat4 model, Vec4 colour, std::uint32_t mesh, std::uint32_t material)
{
    Instance i;
    i.model = model;
    i.colour = colour;
    i.mesh = mesh;
    i.material = material;
    return i;
}

Vec3 randomAxis(Xoshiro256ss &rng)
{
    const float x = uniform(rng, -1.f, 1.f);
    const float y = uniform(rng, -1.f, 1.f);
    const float z = uniform(rng, -1.f, 1.f);
    const Vec3 a = normalize({x, y, z});
    return dot(a, a) > 0.f ? a : Vec3{0, 1, 0};
}

Vec3 lightPosition(float omega, float phi, double t)
{
    const double w = double(omega), p = double(phi);
    return {float(16.0 * std::sin(w * t + p)), float(1.0 + 4.0 * (0.5 + 0.5 * std::sin(1.3 * w * t + p))),
            float(16.0 * std::cos(0.7 * w * t + p))};
}

std::uint64_t totalTriangles(const SceneData &s)
{
    std::uint64_t n = 0;
    for (const Instance &i : s.instances)
        n += triangleCount(s.meshes[i.mesh]);
    return n;
}

SceneData geometry(double contentScale)
{
    SceneData s;
    s.workload = Workload::Geometry;
    Xoshiro256ss rng(0x47454F4Du);
    s.meshes.push_back(displaced(icosphere(int(kGeometryLevels)), 0x47454F4Du, 0.12f, 2.f));
    const std::uint32_t n = scaled(kGeometryInstances, contentScale);
    for (std::uint32_t i = 0; i < n; ++i) {
        const float x = (float(i % 6) - 2.5f) * 2.6f, z = (float(i / 6) - 2.5f) * 2.6f;
        const Vec3 axis = randomAxis(rng);
        const float rate = uniform(rng, 0.3f, 0.9f);
        const float hue = uniform(rng, 0.f, 1.f);
        const Vec3 c = hsv(hue, 0.5f, 0.8f);
        s.instances.push_back(instance(translate({x, 0, z}), {c.x, c.y, c.z, 1.f}, 0, 0));
        s.spins.push_back({axis, rate});
    }
    s.trianglesPerFrame = totalTriangles(s);
    s.drawsPerFrame = std::uint32_t(s.instances.size());
    return s;
}

SceneData drawCalls(double contentScale)
{
    SceneData s;
    s.workload = Workload::DrawCalls;
    Xoshiro256ss rng(0x44524157u);
    s.meshes = {cube(), octahedron(), icosphere(1), icosphere(2), cylinder(24), cone(24), torus(16, 8, 1.f, 0.35f), uvSphere(16, 8),
                grid(1, 1, 200.f, 200.f)};  // the last one: the ground plane
    for (std::uint32_t m = 0; m < 16; ++m)
        s.textures.push_back(checker(m, 64));
    s.instances.push_back(instance(Mat4::identity(), {0.6f, 0.6f, 0.6f, 1.f}, 8, 0));  // drawn first
    const std::uint32_t n = scaled(kDrawObjects, contentScale);
    std::vector<Instance> objects;
    objects.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const float x = (float(i % 120) - 59.5f) * 1.5f, z = (float(i / 120) - 49.5f) * 1.5f;
        const float size = uniform(rng, 0.5f, 0.9f);
        const float angle = uniform(rng, 0.f, float(2.0 * kPi));
        const Mat4 model = translate({x, size, z}) * rotate(angle, {0, 1, 0}) * scale({size, size, size});
        objects.push_back(instance(model, {1.f, 1.f, 1.f, 1.f}, i % 8, (7 * i) % 16));
    }
    for (std::uint32_t i = n; i > 1; --i) {  // Fisher-Yates, seeded
        const std::uint64_t j = rng.below(i);
        std::swap(objects[i - 1], objects[std::size_t(j)]);
    }
    s.instances.insert(s.instances.end(), objects.begin(), objects.end());
    s.trianglesPerFrame = totalTriangles(s);
    s.drawsPerFrame = std::uint32_t(s.instances.size());
    return s;
}

SceneData shading(double contentScale)
{
    SceneData s;
    s.workload = Workload::Shading;
    Xoshiro256ss rng(0x53484144u);
    s.meshes = {room(), icosphere(4), torus(64, 32, 1.f, 0.35f)};
    s.textures = {noiseAlbedo(0x53484144u, 1024), noiseNormal(0x53484145u, 1024), roughMetal(0x53484146u, 1024),
                  aoMap(0x53484147u, 1024)};
    s.instances.push_back(instance(Mat4::identity(), {1.f, 1.f, 1.f, 1.f}, 0, 0));
    s.spins.push_back({});
    const std::uint32_t n = scaled(kShadingObjects, contentScale);
    for (std::uint32_t i = 0; i < n; ++i) {
        const float x = (float(i % 8) - 3.5f) * 4.5f, z = (float(i / 8) - 3.5f) * 4.5f;
        const float y = uniform(rng, 1.5f, 3.5f);
        const float hue = uniform(rng, 0.f, 1.f);
        const float uvScale = uniform(rng, 1.f, 3.f);
        const Vec3 axis = randomAxis(rng);
        const float rate = uniform(rng, 0.1f, 0.4f);
        const Vec3 tint = hsv(hue, 0.4f, 1.f);
        s.instances.push_back(instance(translate({x, y, z}), {tint.x, tint.y, tint.z, uvScale}, 1 + i % 2, 0));
        s.spins.push_back({axis, rate});
    }
    const std::uint32_t lights = scaled(kPointLights, contentScale);
    for (std::uint32_t k = 0; k < lights; ++k) {
        const float omega = uniform(rng, 0.2f, 0.6f);
        const float phi = uniform(rng, 0.f, float(2.0 * kPi));
        const float hue = uniform(rng, 0.f, 1.f);
        const Vec3 c = hsv(hue, 0.7f, 1.f);
        s.lights.push_back({lightPosition(omega, phi, 0.0), 9.f, c, 6.f});
        s.lightMotion.push_back({omega, phi});
    }
    s.trianglesPerFrame = 3 * totalTriangles(s);  // shadow, depth pre-pass, shading
    s.drawsPerFrame = 3 * std::uint32_t(s.instances.size());
    return s;
}

SceneData particles(double contentScale)
{
    SceneData s;
    s.workload = Workload::Particles;
    Xoshiro256ss rng(0x50415254u);
    s.meshes = {grid(1, 1, 60.f, 60.f)};  // the ground; the sky is one full-screen triangle in the shader
    s.textures = {softDisc(64)};
    s.instances.push_back(instance(Mat4::identity(), {0.05f, 0.05f, 0.05f, 1.f}, 0, 0));
    const std::uint32_t n = scaled(kParticles, contentScale);
    s.particles.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        Particle p{};
        p.life = uniform(rng, 2.f, 4.f);
        p.phase = uniform(rng, 0.f, p.life);
        p.speed = uniform(rng, 6.f, 10.f);
        p.azimuth = uniform(rng, 0.f, float(2.0 * kPi));
        p.elevation = uniform(rng, float(60.0 * kPi / 180.0), float(85.0 * kPi / 180.0));
        p.size = uniform(rng, 0.04f, 0.12f);
        p.hue = uniform(rng, 0.f, 0.15f);
        s.particles.push_back(p);
    }
    s.trianglesPerFrame = std::uint64_t(n) * 2 + 1 + 2;  // particles, the sky, the ground
    s.drawsPerFrame = 3 + 14;                            // sky, ground, particles + the post chain
    return s;
}

SceneData quads(double contentScale)
{
    SceneData s;
    s.workload = Workload::Quads;
    s.textures = {fillTexture(0x46494C4Cu)};
    s.quadLayers = scaled(kQuadLayers, contentScale);
    s.trianglesPerFrame = std::uint64_t(s.quadLayers) * 2;
    s.drawsPerFrame = 1;
    return s;
}

SceneData sprites(double contentScale)
{
    SceneData s;
    s.workload = Workload::Sprites;
    Xoshiro256ss rng(0x53505254u);
    s.textures = {spriteAtlas(0x53505254u)};
    const std::uint32_t n = scaled(kSprites, contentScale);
    s.sprites.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        Sprite sp{};
        sp.x0 = uniform(rng, 0.f, 1920.f);
        sp.y0 = uniform(rng, 0.f, 1080.f);
        sp.vx = uniform(rng, -300.f, 300.f);
        sp.vy = uniform(rng, -300.f, 300.f);
        sp.atlasIndex = std::uint32_t(rng.below(16));
        const float hue = uniform(rng, 0.f, 1.f);
        const Vec3 c = hsv(hue, 0.6f, 1.f);
        sp.colourRgba8 = std::uint32_t(c.x * 255.f + 0.5f) | (std::uint32_t(c.y * 255.f + 0.5f) << 8) |
                         (std::uint32_t(c.z * 255.f + 0.5f) << 16) | (255u << 24);
        sp.rotation16 = std::uint32_t(rng.below(65536));
        s.sprites.push_back(sp);
    }
    s.trianglesPerFrame = std::uint64_t(n) * 2;
    s.drawsPerFrame = (n + kSpriteBatch - 1) / kSpriteBatch;
    return s;
}

struct Fnv {
    std::uint64_t h = 0xcbf29ce484222325ull;
    void byte(std::uint8_t b)
    {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    void u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            byte(std::uint8_t(v >> (8 * i)));
    }
    void f(float x) { u32(std::uint32_t(std::int32_t(std::lround(double(x) * 4096.0)))); }  // quantised: a last-bit difference doesn't matter
    void v3(Vec3 v)
    {
        f(v.x);
        f(v.y);
        f(v.z);
    }
    void v4(Vec4 v)
    {
        f(v.x);
        f(v.y);
        f(v.z);
        f(v.w);
    }
};

} // namespace

const char *workloadId(Workload w)
{
    switch (w) {
    case Workload::Geometry: return "geometry";
    case Workload::DrawCalls: return "drawcalls";
    case Workload::Shading: return "shading";
    case Workload::Particles: return "particles";
    case Workload::Quads: return "quads";
    case Workload::Sprites: return "sprites";
    }
    return "geometry";
}

std::optional<Workload> workloadFromId(std::string_view id)
{
    for (const Workload w : kAllWorkloads) {
        if (id == workloadId(w))
            return w;
    }
    return std::nullopt;
}

SceneData buildScene(Workload w, double contentScale)
{
    switch (w) {
    case Workload::Geometry: return geometry(contentScale);
    case Workload::DrawCalls: return drawCalls(contentScale);
    case Workload::Shading: return shading(contentScale);
    case Workload::Particles: return particles(contentScale);
    case Workload::Quads: return quads(contentScale);
    case Workload::Sprites: return sprites(contentScale);
    }
    return {};
}

Camera cameraAt(Workload w, double t, double aspect)
{
    double radius = 0, height = 0;
    Vec3 target{0, 0, 0};
    switch (w) {
    case Workload::Geometry: radius = 14; height = 6; break;
    case Workload::DrawCalls: radius = 70; height = 25; break;
    case Workload::Shading: radius = 15; height = 7; target = {0, 2, 0}; break;
    case Workload::Particles: radius = 18; height = 5; target = {0, 4, 0}; break;
    case Workload::Quads:
    case Workload::Sprites: break;
    }
    Camera c;
    const bool orbits = radius > 0;
    const double theta = 2.0 * kPi * t / 20.0;  // a 20 s loop
    c.eye = orbits ? Vec3{float(radius * std::cos(theta)), float(height), float(radius * std::sin(theta))} : Vec3{0, 0, 0};
    c.target = orbits ? target : Vec3{0, 0, -1};
    c.view = lookAt(c.eye, c.target, {0, 1, 0});
    c.proj = perspective(float(kPi / 3.0), float(aspect), 0.1f, 200.f);
    return c;
}

Mat4 instanceModelAt(const SceneData &s, std::size_t index, double t)
{
    const Mat4 &model = s.instances[index].model;
    if (index >= s.spins.size() || s.spins[index].rate == 0.f)
        return model;
    return model * rotate(float(double(s.spins[index].rate) * t), s.spins[index].axis);
}

std::vector<PointLight> lightsAt(const SceneData &s, double t)
{
    std::vector<PointLight> out = s.lights;
    for (std::size_t k = 0; k < out.size() && k < s.lightMotion.size(); ++k)
        out[k].position = lightPosition(s.lightMotion[k].omega, s.lightMotion[k].phi, t);
    return out;
}

std::uint64_t contentHash(const SceneData &s)
{
    Fnv h;
    h.u32(std::uint32_t(s.workload));
    h.u32(std::uint32_t(s.meshes.size()));
    for (const Mesh &m : s.meshes) {  // meshes: vertices, then indices
        h.u32(std::uint32_t(m.vertices.size()));
        for (const Vertex &v : m.vertices) {
            h.f(v.px); h.f(v.py); h.f(v.pz);
            h.f(v.nx); h.f(v.ny); h.f(v.nz);
            h.f(v.u); h.f(v.v);
        }
        h.u32(std::uint32_t(m.indices.size()));
        for (const std::uint32_t i : m.indices)
            h.u32(i);
    }
    h.u32(std::uint32_t(s.textures.size()));
    for (const Texture &t : s.textures) {
        h.u32(t.width);
        h.u32(t.height);
        h.u32(t.mips ? 1u : 0u);
        for (const std::uint8_t b : t.rgba8)
            h.byte(b);
    }
    h.u32(std::uint32_t(s.instances.size()));
    for (const Instance &i : s.instances) {
        for (const float m : i.model.m)
            h.f(m);
        h.v4(i.colour);
        h.u32(i.mesh);
        h.u32(i.material);
    }
    h.u32(std::uint32_t(s.spins.size()));  // after the instances: the spins are part of them
    for (const Spin &sp : s.spins) {
        h.v3(sp.axis);
        h.f(sp.rate);
    }
    h.u32(std::uint32_t(s.lights.size()));
    for (const PointLight &l : s.lights) {
        h.v3(l.position);
        h.f(l.radius);
        h.v3(l.colour);
        h.f(l.intensity);
    }
    for (const LightMotion &m : s.lightMotion) {  // after the lights
        h.f(m.omega);
        h.f(m.phi);
    }
    h.u32(std::uint32_t(s.particles.size()));
    for (const Particle &p : s.particles) {
        h.f(p.phase); h.f(p.life); h.f(p.speed); h.f(p.azimuth);
        h.f(p.elevation); h.f(p.size); h.f(p.hue); h.f(p.pad);
    }
    h.u32(std::uint32_t(s.sprites.size()));
    for (const Sprite &p : s.sprites) {
        h.f(p.x0); h.f(p.y0); h.f(p.vx); h.f(p.vy);
        h.u32(p.atlasIndex);
        h.u32(p.colourRgba8);
        h.u32(p.rotation16);
        h.u32(p.pad);
    }
    h.u32(s.quadLayers);
    return h.h;
}

} // namespace sb::gpu
