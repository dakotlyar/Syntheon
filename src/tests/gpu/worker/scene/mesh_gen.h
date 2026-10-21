// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Procedural meshes (Phase 4a design §6). Triangles are counter-clockwise seen
// from outside (from inside for room()), vertices carry normals and UVs.

#include "tests/gpu/worker/scene/scenes.h"

namespace sb::gpu {

Mesh icosphere(int levels, float radius = 1.f);  // 20 x 4^levels triangles, shared vertices
// Each vertex moves along its direction to radius (1 + amplitude x fbm(frequency x dir)), 3 octaves;
// normals are recomputed from area-weighted face normals (§6.1).
Mesh displaced(Mesh m, std::uint32_t seed, float amplitude, float frequency);
Mesh cube();                  // 12 triangles
Mesh octahedron();            // 8
Mesh cylinder(int segments);  // 4 x segments (sides and two caps)
Mesh cone(int segments);      // 2 x segments
Mesh torus(int major, int minor, float R, float r);  // 2 x major x minor
Mesh uvSphere(int slices, int stacks);               // 2 x slices x (stacks - 1)
Mesh grid(int cellsX, int cellsZ, float sizeX, float sizeZ);  // in x-z, centred, normal +y
Mesh room();  // §6.3: floor and four walls, 10,240 triangles, normals inward, open top, UV tiling 8x
std::uint64_t triangleCount(const Mesh &m);  // indices / 3

} // namespace sb::gpu
