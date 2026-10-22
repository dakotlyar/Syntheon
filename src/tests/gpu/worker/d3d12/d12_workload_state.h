// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The per-workload state of the D3D12 backend (Phase 4a task 19), shared by
// d12_backend.cpp and d12_workloads.cpp only (Vulkan's vk_workload_state.h).

#include "tests/gpu/worker/d3d12/d12_backend.h"

#include <array>
#include <vector>

namespace sb::gpu::d3d12 {

struct D3D12Backend::WorkloadState {
    Workload workload = Workload::Geometry;
    bool tonemap = false;  // the output is sceneColor, tone mapped by the present pass (mode 0)
    SceneData scene;       // what a frame needs: instances, spins, lights, light motion

    std::vector<Buffer> vertexBuffers, indexBuffers;
    std::vector<std::uint32_t> indexCounts;
    std::vector<Image> materials;    // Draw calls: one per texture; Shading: albedo, normal, rough/metal, AO; Particles: the disc
    std::vector<UINT> materialTables;  // SRV tables (t0-t5)
    Image readTarget;                  // render size, RGBA8: tone mapped, no overlay (§5.7)
    std::uint32_t draws = 0;
    std::uint32_t fillCount = 0;  // task 30: Quads' layers or Sprites' count (w.scene.sprites is the CPU copy; pipeline + materialTables[0])

    ComPtr<ID3D12PipelineState> pipeline;  // Geometry/Draw calls: the scene; Shading: the EQUAL pass; Particles: the ground
    ComPtr<ID3D12PipelineState> shadowPipeline, prepassPipeline;  // Shading
    Buffer particles;                                             // Particles: one Particle (32 bytes) per instance
    std::uint32_t particleCount = 0;
    ComPtr<ID3D12PipelineState> skyPipeline, particlePipeline, brightPipeline, downPipeline, upPipeline, compositePipeline,
        tonemapPipeline, fxaaPipeline;
    // The post passes' SRV tables in pass order: bright, down 1-5, up 5->4 .. 1->0, composite, tone map, FXAA.
    std::array<UINT, 14> postTables{};
};

} // namespace sb::gpu::d3d12
