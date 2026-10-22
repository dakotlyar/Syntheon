// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The D3D12 backend (Phase 4a design §5.3-§5.6, task 18): task 10's structure
// one to one. Two frames in flight on one fence (a value per slot), a command
// allocator per slot, the flip-model swapchain with its frame-latency
// waitable object, timestamps, the present pass with the overlay. Task 18
// draws the clear-only workload (ldrA cleared to (64, 128, 192, 255)); task 19
// adds the four scenes (d12_workloads.cpp, task 19: the Vulkan path's passes,
// constants (scene_constants.h) and shaders; the state transitions mirror
// task 12's barrier table, see the design Log). Windows only (P-WIN).
//
// The one root signature (common.hlsli's table; every pipeline uses it):
//   0  root CBV b0  Frame (the slot's frameConstants at 0)
//   1  root CBV b1  Object (task 19: the slot's objectConstants + 256 i per draw)
//   2  root CBV b2  Lights / Post / Present (the slot's frameConstants at a 256-byte block)
//   3  table t0-t5  SRVs (t4 the shadow map, t5 the glyph atlas; null SRVs where unused)
//   static samplers s0 anisotropic 8 wrap, s1 linear clamp, s2 comparison LESS_EQUAL (border white)

#include "tests/gpu/worker/d3d12/d12_resources.h"

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace sb::gpu::d3d12 {

// The per-slot frame constants (as Vulkan's, vk_backend.h): Frame at 0, then
// 256-byte pass blocks (Present at kPassBase); DX-4: CBVs on 256-byte boundaries.
inline constexpr UINT64 kFrameConstantsBytes = 64 * 1024, kFrameBlock = 256, kPassBase = 256;

class D3D12Backend final : public IBackend
{
public:
    D3D12Backend();
    ~D3D12Backend() override;
    D3D12Backend(const D3D12Backend &) = delete;
    D3D12Backend &operator=(const D3D12Backend &) = delete;

    Api api() const override { return Api::Dx12; }
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
    void setRenderSize(std::uint32_t w, std::uint32_t h) override { m_init.renderWidth = w, m_init.renderHeight = h; }

    // Everything released (also by the destructor); the validation counters stay readable.
    void shutdown();
    int validationErrors() const { return m_ctx.validationErrors.load(); }
#if defined(SYNBENCH_DEV_HOOKS)
    // Dev hook, headless only (task 18's overlay test, as Vulkan's): the last
    // frame's present-pass output (render size, RGBA8, top row first), after drain().
    std::string readbackPresented(std::vector<std::uint8_t> *rgba8);
#endif

private:
    struct Slot { ComPtr<ID3D12CommandAllocator> allocator; UINT64 fenceValue = 0; ComPtr<ID3D12Resource> frameConstants, objectConstants, spriteRing; void *mapped[3]{};
                  bool submitted = false; };
    Context m_ctx; dxgi::Swapchain m_swap; std::array<Slot, 2> m_slots; ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<ID3D12QueryHeap> m_queries; ComPtr<ID3D12Resource> m_queryReadback;
    ComPtr<ID3D12RootSignature> m_root;   // the one root signature: b0 CBV, b1 CBV (root descriptor, per-draw address), b2 CBV, t0-t5 table, s0-s2 static samplers
    // The targets of §5.3 with their tracked states (d12_resources.h's Image).
    Image m_ldrA, m_headlessTarget, m_atlas;
    Image m_sceneColor, m_sceneDepth, m_hdrB, m_ldrB, m_shadow; std::array<Image, 6> m_bloom;  // task 19, made in load()
    struct WorkloadState; std::unique_ptr<WorkloadState> m_work;   // per-workload pipelines/buffers/tables (d12_workloads.cpp)

    // Beyond the declaration in task 18.
    std::string makePresentPipeline(DXGI_FORMAT format, ComPtr<ID3D12PipelineState> *out);
    std::string waitSlot(std::uint32_t slot, std::vector<double> *gpuMs);  // gpuMs null: drop its timestamps
    std::string waitAll();
    // Records the workload at t (the slot supplies its constants) and leaves its
    // output (outputImage()) a render target; sets m_lastDraws.
    void recordWorkload(ID3D12GraphicsCommandList *list, Slot &slot, double t);
    void recordPresentPass(ID3D12GraphicsCommandList *list, Slot &slot, D3D12_CPU_DESCRIPTOR_HANDLE rtv, std::uint32_t w, std::uint32_t h,
                           ID3D12PipelineState *pipeline);
    // `pre` records first (the workload, and for tone mapped ones the present pass into the read target); then img is copied out.
    std::string readImage(Image &img, const std::function<void(ID3D12GraphicsCommandList *)> &pre, std::vector<std::uint8_t> *rgba8);

    // d12_workloads.cpp (task 19): the four scenes and the post chain.
    bool tonemapped() const;   // the output is sceneColor (Present mode 0)
    Image &outputImage();      // sceneColor, ldrA, or ldrB (Particles)
    std::string loadScene(const SceneData &scene);      // Geometry, Draw calls
    std::string loadShading(const SceneData &scene);
    std::string loadParticles(const SceneData &scene);
    void unloadScene();
    void recordScene(ID3D12GraphicsCommandList *list, Slot &slot, double t);
    void recordShading(ID3D12GraphicsCommandList *list, Slot &slot, double t);
    void recordParticles(ID3D12GraphicsCommandList *list, Slot &slot, double t);
    void postPass(ID3D12GraphicsCommandList *list, Slot &slot, Image &target, bool accumulate, bool toShaderRead, ID3D12PipelineState *pipeline,
                  UINT table, std::uint32_t block);
    // A render target (rtv) or depth buffer (dsv) of the load region; depth formats: R32_TYPELESS (sampled) or D32_FLOAT.
    std::string makeTarget(Image &img, std::uint32_t w, std::uint32_t h, DXGI_FORMAT format, bool depth, const float *clearColour);
    // A 6-descriptor SRV table (t0-t5) of the load region: the images in order, null SRVs after them.
    UINT makeTable(std::initializer_list<const Image *> images);
    void releaseWindow();

    BackendInit m_init;
    bool m_headless = false;
    ComPtr<ID3D12PipelineState> m_presentPipeline;
    ComPtr<ID3D12PipelineState> m_readPipeline;   // the present pass into an RGBA8 target (the tone mapped readback)
    // The load() .. unload() regions of the descriptor heaps (after the fixed slots).
    UINT m_rtvNext = 0, m_dsvNext = 0, m_srvNext = 0;
    DXGI_FORMAT m_presentFormat = DXGI_FORMAT_UNKNOWN;
    std::array<ComPtr<ID3D12Resource>, dxgi::Swapchain::kBuffers> m_backBuffers;
    UINT m_presentTable = 0;  // the present pass's SRV table (t0 = ldrA ... t5 = the atlas), from load()
    UINT64 *m_timestamps = nullptr;  // m_queryReadback, mapped: 2 per slot
    std::uint64_t m_frame = 0;
    std::uint32_t m_lastDraws = 0;
    std::string m_overlay, m_presentModeName;
    double m_windowScale = 1.0;
    bool m_loaded = false, m_resumed = false, m_everResumed = false;
};

} // namespace sb::gpu::d3d12
