// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The shared scene description (Phase 4a design §5.1, §6): plain data for the
// six workloads, built once per worker process from fixed seeds. Backends
// upload what SceneData says and never make content decisions. Qt-free.

#include "tests/gpu/worker/scene/math.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace sb::gpu {

enum class Workload : std::uint8_t { Geometry, DrawCalls, Shading, Particles, Quads, Sprites };
inline constexpr std::array<Workload, 6> kAllWorkloads = {Workload::Geometry, Workload::DrawCalls, Workload::Shading,
                                                         Workload::Particles, Workload::Quads, Workload::Sprites};
const char *workloadId(Workload w);  // "geometry","drawcalls","shading","particles","quads","sprites"
std::optional<Workload> workloadFromId(std::string_view id);

struct Vertex {
    float px, py, pz, nx, ny, nz, u, v;  // 32 bytes, all meshes
};
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};
struct Texture {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba8;  // level 0; backends build mips with the shared box filter (mipChain)
    bool mips = true;
};
struct Instance {
    Mat4 model;
    Vec4 colour;  // Shading: rgb = the object's tint, w = its texture UV scale
    std::uint32_t mesh = 0, material = 0;  // material = texture index
};
struct PointLight {
    Vec3 position;  // at t = 0; lightsAt() gives it at any t (§6.3)
    float radius;
    Vec3 colour;
    float intensity;
};
struct Particle {
    float phase, life, speed, azimuth, elevation, size, hue, pad;  // 32 bytes; motion is evaluated in the vertex shader
};
struct Sprite {
    float x0, y0, vx, vy;
    std::uint32_t atlasIndex, colourRgba8, rotation16, pad;
};

// Additions to §5.1 (the design's animation needs data the structs above have
// no room for; reported with task 8). Both are parallel to the vector they
// extend and are part of the content hash.
struct Spin {  // an instance's own rotation: model * rotate(rate * t, axis); rate 0 = none
    Vec3 axis{0, 1, 0};
    float rate = 0;
};
struct LightMotion {  // §6.3: position = f(omega, phi, t)
    float omega = 0, phi = 0;
};

struct SceneData {
    Workload workload = Workload::Geometry;
    std::vector<Mesh> meshes;         // indexed by Instance::mesh
    std::vector<Texture> textures;    // indexed by Instance::material and by the passes
    std::vector<Instance> instances;  // in draw order
    std::vector<Spin> spins;          // parallel to instances (empty = no instance spins)
    std::vector<PointLight> lights;   // Shading only
    std::vector<LightMotion> lightMotion;  // parallel to lights
    std::vector<Particle> particles;  // Particles only
    std::vector<Sprite> sprites;      // Sprites only
    std::uint32_t quadLayers = 0;     // Quads only
    // Totals the tests check (§6): what one frame draws.
    std::uint64_t trianglesPerFrame = 0;
    std::uint32_t drawsPerFrame = 0;  // excluding the present pass
};

struct Camera {
    Vec3 eye, target;
    Mat4 view, proj;  // proj: 60 degrees vertical, near 0.1, far 200, depth 0..1
};

// Content scale (dev hook --content-scale, 1.0 in packaged builds) divides the
// instance, light, particle, sprite and layer counts (rounded up, at least 1).
SceneData buildScene(Workload w, double contentScale = 1.0);
// The scene's camera at animation time t (a 20 s loop, §6). Quads and Sprites
// have none: the default camera at the origin.
Camera cameraAt(Workload w, double t, double aspect);
// An instance's model matrix at time t (its base model spun by its Spin).
Mat4 instanceModelAt(const SceneData &s, std::size_t index, double t);
// Shading: the light positions at time t.
std::vector<PointLight> lightsAt(const SceneData &s, double t);
// FNV-1a 64 over the quantised scene data (§6.7).
std::uint64_t contentHash(const SceneData &s);

// Scene constants (the only place they live; D15 may change them).
inline constexpr std::uint32_t kGeometryInstances = 36, kGeometryLevels = 7, kDrawObjects = 38400, kShadingObjects = 64,
                               kPointLights = 128, kParticles = 2359296, kQuadLayers = 128, kSprites = 100000,
                               kSpriteBatch = 4096;

} // namespace sb::gpu
