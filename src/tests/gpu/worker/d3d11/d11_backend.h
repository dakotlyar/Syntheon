// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The D3D11 backend (Phase 4a design §5.3-§5.6, task 20): task 18's D3D12
// backend mapped to the immediate context. One device context, no states to
// track; the runtime queues the frames, so the 2 frames in flight (P4) come
// from the swapchain's frame-latency waitable object, IDXGIDevice1::
// SetMaximumFrameLatency(2) and the per-slot timestamp queries (a frame's
// results are waited for before its slot is reused). Per-object constants
// are one DYNAMIC buffer written with Map(WRITE_DISCARD) once per frame and
// bound per draw through VSSetConstantBuffers1 / PSSetConstantBuffers1 with a
// first-constant offset of 16 i (256 B = 16 constants), which needs D3D11.1's
// ConstantBufferOffsetting (else N/A). The slot-to-binding map of
// common.hlsli: b0 Frame (the frame buffer at 0), b1 Object (the object
// buffer + 256 i), b2 Lights / Post / Present (the frame buffer at a
// 256-byte block), t0-t5 SRVs (t4 the shadow map, t5 the glyph atlas), s0-s2
// samplers. DX11-1: an SRV is unbound before its texture is a render target.
// Windows only (P-WIN).

#include "tests/gpu/worker/dxgi/dxgi_swapchain.h"

#include <d3d11_1.h>
#include <d3d11sdklayers.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace sb::gpu::d3d11 {

using Microsoft::WRL::ComPtr;

// A texture with the views it is used through (the shadow map: R32_TYPELESS
// with a D32_FLOAT DSV and an R32_FLOAT SRV).
struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    std::uint32_t width = 0, height = 0;
};
// t0-t5 of one pass; null SRVs after the images given.
using Table = std::array<ComPtr<ID3D11ShaderResourceView>, 6>;

// Shaders, layout and the fixed-function state of one pipeline (D3D12's PSO).
struct Pipeline {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;  // null: depth only
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11BlendState> blend;
};
struct PipelineDesc {
    const unsigned char *vs = nullptr, *ps = nullptr;  // DXBC (SM 5.0); ps null: no pixel shader
    std::size_t vsSize = 0, psSize = 0;
    enum class Input { None, Mesh, Particle, Sprite } input = Input::None;
    bool depthTest = false, depthWrite = false;
    D3D11_COMPARISON_FUNC compare = D3D11_COMPARISON_LESS;
    bool additive = false;   // blend ONE, ONE
    bool alphaBlend = false; // blend SRC_ALPHA, INV_SRC_ALPHA (colour), ONE, INV_SRC_ALPHA (alpha): the fill workloads
    bool depthBias = false;  // §6.3: DepthBias 0, SlopeScaledDepthBias 2.0 (Vulkan: vkCmdSetDepthBias(0, 0, 2))
};
std::string makePipeline(ID3D11Device *device, const PipelineDesc &desc, Pipeline *out);

// The per-slot frame constants (as Vulkan's and D3D12's): Frame at 0, then
// 256-byte pass blocks (Present at kPassBase), 64 KiB.
inline constexpr UINT kFrameConstantsBytes = 64 * 1024, kFrameBlock = 256, kPassBase = 256;

class D3D11Backend final : public IBackend
{
public:
    D3D11Backend();
    ~D3D11Backend() override;
    D3D11Backend(const D3D11Backend &) = delete;
    D3D11Backend &operator=(const D3D11Backend &) = delete;

    Api api() const override { return Api::Dx11; }
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
    int validationErrors() const { return m_validationErrors; }
#if defined(SYNBENCH_DEV_HOOKS)
    // Dev hook, headless only (task 20's overlay test, as Vulkan's and D3D12's): the
    // last frame's present-pass output (render size, RGBA8, top row first), after drain().
    std::string readbackPresented(std::vector<std::uint8_t> *rgba8);
#endif

private:
    struct WorkloadState;  // d11_workload_state.h
    // Two timestamps and the disjoint query of one frame slot (§5.6).
    struct QuerySlot {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };

    // d11_backend.cpp
    std::string createDevice(const BackendInit &init, BackendInfo *info);
    std::string validationProblem();
    std::string removedReason();
    std::string waitSlot(std::uint32_t slot, std::vector<double> *gpuMs);  // gpuMs null: drop its timestamps
    std::string waitAll();
    void releaseWindow();
    // A texture from the scene's CPU side (the mip chain when it has mips), immutable.
    std::string uploadTexture(const Texture &level0, Image *out);
    // Immutable vertex / index / instance data.
    std::string uploadBuffer(const void *data, std::size_t bytes, UINT bindFlags, ComPtr<ID3D11Buffer> *out);
    // Bindings (the immediate context keeps them between calls; each recording starts with bindCommon()).
    void bindCommon();
    void bindPipeline(const Pipeline &p);
    // b<slot> to `bytes` of `buffer` at byteOffset (16-byte units through the *1 calls); ps false: vertex stage only.
    void setConstants(UINT slot, ID3D11Buffer *buffer, UINT byteOffset, UINT bytes, bool ps = true);
    void bindTable(const Table &t);
    void unbindSrvs();  // DX11-1
    void setViewport(std::uint32_t w, std::uint32_t h);
    std::string flushFrameConstants();  // m_frameShadow -> the frame buffer, Map(WRITE_DISCARD)
    // Records the workload at t and leaves its output (outputImage()) the bound render target; sets m_lastDraws.
    std::string recordWorkload(double t);
    void recordPresentPass(ID3D11RenderTargetView *rtv, std::uint32_t w, std::uint32_t h);
    std::string readImage(Image &img, std::vector<std::uint8_t> *rgba8);

    // d11_workloads.cpp (the four scenes and the post chain)
    bool tonemapped() const;   // the output is sceneColor (Present mode 0)
    Image &outputImage();      // sceneColor, ldrA, or ldrB (Particles)
    std::string loadScene(const SceneData &scene);      // Geometry, Draw calls
    std::string loadShading(const SceneData &scene);
    std::string loadParticles(const SceneData &scene);
    std::string recordScene(double t);
    std::string recordShading(double t);
    std::string recordParticles(double t);
    // The frame constants and the objects of scene time t (scene_constants.h), then the upload of both.
    std::string writeConstants(double t);
    std::uint32_t drawObjects(bool materials);
    // Task 30: Quads and Sprites into ldrA (§6.5, §6.6).
    std::string loadFill(const SceneData &scene);
    std::string recordFill(double t);
    void postPass(Image &target, const Pipeline &pipeline, const Table &table, std::uint32_t block);
    std::string uploadMeshes(const SceneData &scene);  // every mesh's immutable vertex and index buffers
    // A render target (rtv + srv), depth buffer (dsv) or sampled depth buffer (dsv + srv, R32_TYPELESS).
    enum class Kind { Colour, Depth, SampledDepth };
    std::string makeTarget(Image &img, std::uint32_t w, std::uint32_t h, DXGI_FORMAT format, Kind kind);
    Table makeTable(std::initializer_list<const Image *> images);

    BackendInit m_init;
    bool m_headless = false;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D11Device1> m_device;
    ComPtr<ID3D11DeviceContext1> m_cx;
    ComPtr<ID3D11InfoQueue> m_info;   // the debug layer's messages (--validation), else null
    int m_validationErrors = 0;
    std::string m_firstValidationError;
    dxgi::Swapchain m_swap;
    ComPtr<ID3D11Texture2D> m_backTexture;
    ComPtr<ID3D11RenderTargetView> m_backRtv;
    Image m_ldrA, m_headlessTarget, m_atlas;
    Image m_sceneColor, m_sceneDepth, m_hdrB, m_ldrB, m_shadow;
    std::array<Image, 6> m_bloom;
    std::unique_ptr<WorkloadState> m_work;
    std::array<ComPtr<ID3D11SamplerState>, 4> m_samplers;  // s0 anisotropic 8 wrap, s1 linear clamp, s2 comparison, s3 plain bilinear wrap
    ComPtr<ID3D11Buffer> m_frameCb, m_objectCb, m_spriteCb;  // m_spriteCb: the Sprites' DYNAMIC instance buffer (Map(WRITE_DISCARD) once per frame)
    std::vector<std::uint8_t> m_frameShadow;  // the CPU side of the frame buffer
    Pipeline m_presentPipeline;
    Table m_presentTable;  // t0 = the workload's output ... t5 = the atlas
    std::array<QuerySlot, 2> m_queries;
    bool m_timestamps = false, m_psBound = true;
    std::uint64_t m_frame = 0;
    std::uint32_t m_lastDraws = 0;
    std::string m_overlay, m_presentModeName;
    double m_windowScale = 1.0;
    bool m_loaded = false, m_resumed = false, m_everResumed = false;
};

} // namespace sb::gpu::d3d11
