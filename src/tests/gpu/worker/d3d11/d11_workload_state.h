// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The per-workload state of the D3D11 backend (Phase 4a task 20), shared by
// d11_backend.cpp and d11_workloads.cpp only (d12_workload_state.h's counterpart).

#include "tests/gpu/worker/d3d11/d11_backend.h"

#include <array>
#include <vector>

namespace sb::gpu::d3d11 {

struct D3D11Backend::WorkloadState {
    Workload workload = Workload::Geometry;
    bool tonemap = false;  // the output is sceneColor, tone mapped by the present pass (mode 0)
    SceneData scene;       // what a frame needs: instances, spins, lights, light motion

    std::vector<ComPtr<ID3D11Buffer>> vertexBuffers, indexBuffers;
    std::vector<std::uint32_t> indexCounts;
    std::vector<Image> materials;    // Draw calls: one per texture; Shading: albedo, normal, rough/metal, AO; Particles: the disc
    std::vector<Table> materialTables;
    Image readTarget;                // render size, RGBA8: tone mapped, no overlay (§5.7)
    std::uint32_t draws = 0;
    std::uint32_t fillCount = 0;  // Quads' layers or Sprites' count (scene.sprites is the CPU copy; pipeline + materialTables[0])

    Pipeline pipeline;  // Geometry/Draw calls: the scene; Shading: the EQUAL pass; Particles: the ground
    Pipeline shadowPipeline, prepassPipeline;  // Shading
    ComPtr<ID3D11Buffer> particles;            // Particles: one Particle (32 bytes) per instance
    std::uint32_t particleCount = 0;
    Pipeline skyPipeline, particlePipeline, brightPipeline, downPipeline, upPipeline, compositePipeline, tonemapPipeline, fxaaPipeline;
    // The post passes' tables in pass order: bright, down 1-5, up 5->4 .. 1->0, composite, tone map, FXAA.
    std::array<Table, 14> postTables;
};

} // namespace sb::gpu::d3d11
