// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The per-workload state of the Vulkan backend (Phase 4a task 11), shared by
// vk_backend.cpp (load/unload/frame dispatch) and vk_workloads.cpp (the
// Geometry and Draw calls workloads; tasks 12 and 29 add theirs). Private to
// the backend: only these two files include it.

#include "tests/gpu/worker/vulkan/vk_backend.h"

#include <array>
#include <vector>

namespace sb::gpu::vk {

struct VulkanBackend::WorkloadState {
    Workload workload = Workload::Geometry;
    // True for the workloads that render into sceneColor (RGBA16F HDR) and are
    // tone mapped by the present pass (mode 0); false for the ones that draw
    // display-encoded colour into ldrA (the clear workload, Quads, Sprites; Copy, mode 1).
    bool tonemap = false;

    // Geometry and Draw calls (tasks 11): what a frame needs of the scene.
    SceneData scene;  // instances and spins only (instanceModelAt)
    std::vector<Buffer> vertexBuffers, indexBuffers;
    std::vector<std::uint32_t> indexCounts;
    std::vector<Image> materials;                 // set 2 b0, one per texture
    std::vector<VkDescriptorSet> materialSets;
    std::array<VkDescriptorSet, 2> objectSets{};  // set 1 per slot (b0 = the slot's objectConstants)
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline readPipeline = VK_NULL_HANDLE;     // the present pass into the RGBA8 readback target
    Image readTarget;                             // render size, RGBA8: tone mapped, no overlay
    std::uint32_t draws = 0;

    // Shading (task 12): scene.lights/lightMotion too (lightsAt); materials[0..3]
    // are albedo, normal, roughness/metal and AO in materialSets[0]; the shadow
    // map is the backend's m_shadow (set 0 b2). `pipeline` is the EQUAL shading pass.
    VkPipeline shadowPipeline = VK_NULL_HANDLE, prepassPipeline = VK_NULL_HANDLE;

    // Particles + post (task 12): the ground uses geometry.hlsl (pipeline), the
    // sky and the particles their own; materials[0] is the soft disc (materialSets[0]).
    Buffer particles;  // one Particle (32 bytes) per instance
    std::uint32_t particleCount = 0;
    VkPipeline skyPipeline = VK_NULL_HANDLE, particlePipeline = VK_NULL_HANDLE;
    VkPipeline brightPipeline = VK_NULL_HANDLE, downPipeline = VK_NULL_HANDLE, upPipeline = VK_NULL_HANDLE,
               compositePipeline = VK_NULL_HANDLE, tonemapPipeline = VK_NULL_HANDLE, fxaaPipeline = VK_NULL_HANDLE;
    // Set 2 per post pass, in pass order: bright, down 1-5, up 5->4 .. 1->0, composite, tonemap, FXAA.
    std::array<VkDescriptorSet, 14> postSets{};
};

} // namespace sb::gpu::vk
