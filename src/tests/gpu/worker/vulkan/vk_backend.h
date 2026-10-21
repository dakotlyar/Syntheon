// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Vulkan backend (Phase 4a design §5.3-§5.6, task 10): two frames in
// flight, synchronization2 barriers, dynamic rendering, GPU timestamps, the
// present pass with the overlay. Task 10 draws the clear-only workload
// (ldrA cleared to (64, 128, 192, 255), nothing drawn); tasks 11, 12 and 29
// add the real workloads in vk_workloads.cpp (WorkloadState).
//
// Shared binding layout (common.hlsli; every Vulkan pipeline uses
// m_presentLayout):
//   set 0: b0 Frame (UBO, the slot's frameConstants at 0, 256 B)
//          b1 Lights / Post / Present (UBO_DYNAMIC, the slot's frameConstants,
//             range kPassRange; the dynamic offset picks the pass's block)
//          b2 shadow map, b3 glyph atlas (sampled images)
//          b4 s0 linear wrap aniso 8, b5 s1 linear clamp, b6 s2 comparison (immutable samplers)
//   set 1: b0 Object (UBO_DYNAMIC, per-draw offset; task 11)
//   set 2: b0-b3 textures (sampled images; the present pass's source is b0)

#include "tests/gpu/worker/backend.h"
#include "tests/gpu/worker/vulkan/vk_context.h"
#include "tests/gpu/worker/vulkan/vk_resources.h"
#include "tests/gpu/worker/vulkan/vk_swapchain.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sb::gpu::vk {

// The per-slot frame constants buffer: Frame at 0, then 256-byte pass blocks
// (dynamic offsets on set 0 binding 1), each read with kPassRange.
inline constexpr VkDeviceSize kFrameConstantsBytes = 64 * 1024, kFrameBlock = 256, kPassBase = 256, kPassRange = 4096;

class VulkanBackend final : public IBackend
{
public:
    VulkanBackend();
    ~VulkanBackend() override;
    VulkanBackend(const VulkanBackend &) = delete;
    VulkanBackend &operator=(const VulkanBackend &) = delete;

    Api api() const override { return Api::Vulkan; }
    std::string init(const BackendInit &init, const PresentTarget &target, BackendInfo *info) override;
    std::string load(const SceneData &scene) override;
    std::string frame(double t, std::vector<double> *gpuMs) override;
    std::string drain(std::vector<double> *gpuMs) override;
    std::string readback(double t, std::vector<std::uint8_t> *rgba8) override;
    void unload() override;
    std::string suspend() override;
    std::string resume(const PresentTarget &target) override;
    void setOverlay(const std::string &text) override { m_overlay = text; }
    std::uint32_t lastFrameDraws() const override { return m_lastDraws; }
    std::string presentMode() const override { return m_presentModeName; }

    // Everything released (also by the destructor); the validation counters stay readable.
    void shutdown();
    int validationErrors() const { return m_ctx.validationErrors.load(); }
#if defined(SYNBENCH_DEV_HOOKS)
    // Dev hook, headless only (task 10's overlay test): the last frame's
    // present-pass output (render size, RGBA8, top row first), after drain().
    std::string readbackPresented(std::vector<std::uint8_t> *rgba8);
#endif

private:
    struct Slot { VkCommandPool pool{}; VkCommandBuffer cmd{}; VkFence fence{}; VkSemaphore acquired{}; bool submitted = false;
                  Buffer frameConstants, objectConstants, spriteRing;
                  VkDescriptorSet set0{}; };
    Context m_ctx; Swapchain m_swap; bool m_headless = false;
    std::array<Slot, 2> m_slots; std::uint64_t m_frame = 0; VkQueryPool m_queries{};
    Image m_sceneColor, m_sceneDepth, m_hdrB, m_ldrA, m_ldrB, m_shadow, m_headlessTarget; std::array<Image, 6> m_bloom;
    VkPipeline m_presentPipeline{}; VkPipelineLayout m_presentLayout{}; VkDescriptorPool m_pool{};
    struct WorkloadState; std::unique_ptr<WorkloadState> m_work;   // per-workload pipelines/buffers (tasks 11, 12, 29)
    std::uint32_t m_lastDraws = 0;

    // Beyond the declaration in task 10.
    std::string createPresentPipeline(VkFormat format);
    std::string waitSlot(std::uint32_t slot, std::vector<double> *gpuMs);  // gpuMs null: drop its timestamps
    std::string waitAll();
    // Records the workload at time t into the slot's command buffer and leaves
    // its output (outputImage()) as a colour attachment; sets m_lastDraws.
    void recordWorkload(VkCommandBuffer cmd, Slot &slot, double t);
    void recordPresentPass(VkCommandBuffer cmd, Slot &slot, VkImageView view, std::uint32_t w, std::uint32_t h, VkPipeline pipeline);
    void writePresentConstants(Slot &slot, std::uint32_t windowW, std::uint32_t windowH, std::uint32_t mode, bool withOverlay);
    // `pre` records first (the workload, and for tone mapped ones the present pass into the read target); then img is copied out.
    std::string readImage(Image &img, const std::function<void(VkCommandBuffer)> &pre, std::vector<std::uint8_t> *rgba8);
    std::string buildPresentPipeline(VkFormat format, VkPipeline *out);

    // vk_workloads.cpp (task 11): Geometry and Draw calls into sceneColor, tone mapped.
    bool tonemapped() const;          // the workload renders into sceneColor (present mode 0)
    Image &outputImage();             // sceneColor or ldrA
    std::string loadScene(const SceneData &scene);   // the targets, pipeline and buffers of a scene workload
    void unloadScene();
    void recordScene(VkCommandBuffer cmd, Slot &slot, double t);
    void releaseWindow();

    BackendInit m_init;
    std::array<VkDescriptorSetLayout, 3> m_setLayouts{};
    std::array<VkSampler, 3> m_samplers{};
    Image m_atlas;
    VkFormat m_presentFormat = VK_FORMAT_UNDEFINED;
    VkDescriptorPool m_loadPool{};      // load() .. unload(): descriptor sets on the render targets
    VkDescriptorSet m_presentSource{};  // set 2 for the present pass: b0 = the workload's output
    std::string m_overlay, m_presentModeName;
    double m_windowScale = 1.0;
    bool m_loaded = false, m_resumed = false;
};

} // namespace sb::gpu::vk
