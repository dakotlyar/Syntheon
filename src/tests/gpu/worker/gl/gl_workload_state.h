// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The per-workload state of the OpenGL backend (Phase 4a task 22), shared by
// gl_backend.cpp and gl_workloads.cpp only (d11_workload_state.h's counterpart).
// Every GL object here is freed by GlBackend::releaseWorkload() (the context current).

#include "tests/gpu/worker/gl/gl_backend.h"
#include "tests/gpu/worker/scene_constants.h"

#include <vector>

namespace sb::gpu::gl {

struct GlBackend::WorkloadState {
    Workload workload = Workload::Geometry;
    bool tonemap = false;  // the output is sceneColor, tone mapped by the present pass (mode 0)
    SceneData scene;       // what a frame needs: instances, spins, lights, light motion

    std::vector<std::uint32_t> vertexBuffers, indexBuffers, meshVaos;  // per mesh; the VAO holds both buffers
    std::vector<std::uint32_t> indexCounts;
    std::vector<std::uint32_t> materials;  // textures: Draw calls one per texture; Shading albedo, normal, rough/metal, AO; Particles the disc
    Target readTarget;                     // render size, RGBA8: tone mapped, no overlay (§5.7)
    std::uint32_t draws = 0;

    std::vector<std::uint32_t> fbos;  // made by makeFbo()
    std::uint32_t sceneFbo = 0, prepassFbo = 0, shadowFbo = 0;  // colour + depth, depth only (sceneDepth), depth only (shadow map)
    Program scene3d;  // Geometry/Draw calls: the scene; Shading: the EQUAL pass; Particles: the ground
    Program depthOnly;  // Shading: the shadow and the pre-pass (DepthPass block)
    Program sky, particle, bright, down, up, composite, toneMap, fxaa;
    std::uint32_t particleBuffer = 0, particleVao = 0, particleCount = 0;
    std::uint32_t spriteBuffer = 0, spriteVao = 0, fillCount = 0;  // Quads' layers or Sprites' count; scene3d is the fill program, materials[0] its texture
    std::vector<sc::SpriteBatch> spriteBatches;  // the Sprites' batches, made once at load (no allocation in the timed loop)
};

} // namespace sb::gpu::gl
