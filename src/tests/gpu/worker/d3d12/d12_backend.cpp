// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/d3d12/d12_backend.h"

#include "tests/gpu/worker/d3d12/d12_workload_state.h"

#include "tests/gpu/worker/present_constants.h"
#include "tests/gpu/worker/scene_constants.h"
#include "tests/gpu/worker/scene/bitmap_font.h"

#include <cfloat>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {
// The Windows build's DXIL (worker/shaders/CMakeLists.txt, SYNBENCH_GPU_SHADER_DIR).
#include "present_ps.dxil.inc"
#include "present_vs.dxil.inc"
} // namespace

namespace sb::gpu::d3d12 {

static_assert(kPassBase == sc::kPresentOffset && kFrameConstantsBytes == sc::kFrameConstantsBytes && kFrameBlock == sc::kBlock,
              "D3D12's frame-constants layout is scene_constants.h's (and Vulkan's)");

namespace {

// The clear-only workload of task 18 (task 10's, §5.3 Copy mode): ldrA = (64, 128, 192, 255).
constexpr float kClearColour[4] = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};

// Fixed RTV heap slots: the swapchain's back buffers, the headless target, ldrA.
constexpr UINT kRtvBackBuffer = 0, kRtvHeadless = dxgi::Swapchain::kBuffers, kRtvLdrA = kRtvHeadless + 1;
// The SRV tables are 6 descriptors (t0-t5); the present pass's is at 0. The
// load() .. unload() regions follow the fixed slots (task 19's targets and tables).
constexpr UINT kTableSize = 6, kPresentTableIndex = 0, kRtvLoadBase = kRtvLdrA + 1, kSrvLoadBase = kTableSize, kDsvLoadBase = 0;

D3D12_STATIC_SAMPLER_DESC staticSampler(UINT reg, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE address)
{
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = filter;
    s.AddressU = s.AddressV = s.AddressW = address;
    s.MipLODBias = 0.0f;
    s.MaxAnisotropy = 1;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    s.MinLOD = 0.0f;
    s.MaxLOD = FLT_MAX;
    s.ShaderRegister = reg;
    s.RegisterSpace = 0;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return s;
}

} // namespace

D3D12Backend::D3D12Backend() = default;

D3D12Backend::~D3D12Backend()
{
    shutdown();
}

void D3D12Backend::shutdown()
{
    if (m_ctx.device) {
        waitAll();
        unload();
        releaseWindow();
    }
    for (Slot &s : m_slots)
        s = Slot{};
    m_list.Reset();
    m_queries.Reset();
    m_queryReadback.Reset();
    m_timestamps = nullptr;
    m_presentPipeline.Reset();
    m_readPipeline.Reset();
    m_root.Reset();
    m_atlas = Image{};
    m_ctx.destroy();
}

std::string D3D12Backend::init(const BackendInit &init, const PresentTarget &target, BackendInfo *info)
{
    m_init = init;
    m_headless = target.headless;
    if (std::string e = m_ctx.create(init, init.validation, info); !e.empty())
        return e;
    ID3D12Device *d = m_ctx.device.Get();

    // The one root signature (d12_backend.h).
    D3D12_DESCRIPTOR_RANGE srvs{};
    srvs.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvs.NumDescriptors = kTableSize;
    srvs.BaseShaderRegister = 0;
    srvs.RegisterSpace = 0;
    srvs.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER params[4]{};
    for (UINT b = 0; b < 3; ++b) {
        params[b].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[b].Descriptor.ShaderRegister = b;
        params[b].Descriptor.RegisterSpace = 0;
        params[b].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &srvs;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samplers[3] = {
        staticSampler(0, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
        staticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
        staticSampler(2, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER),
    };
    samplers[0].MaxAnisotropy = 8;
    samplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[2].MaxLOD = 0.0f;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 3;
    rsd.pStaticSamplers = samplers;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, errors;
    if (const HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors); FAILED(hr)) {
        std::string why = dxgi::failure("D3D12SerializeRootSignature", hr);
        if (errors)
            why += std::string(": ") + static_cast<const char *>(errors->GetBufferPointer());
        return why;
    }
    if (const HRESULT hr = d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root)); FAILED(hr))
        return dxgi::failure("CreateRootSignature", hr);

    // The glyph atlas, once (§5.9).
    if (std::string e = uploadTexture(m_ctx, glyphAtlas(), &m_atlas); !e.empty())
        return e;

    // Per slot: a command allocator (DX-2) and the frame constants (an UPLOAD
    // buffer, persistently mapped; DX-4: 256-byte blocks).
    for (Slot &s : m_slots) {
        if (const HRESULT hr = d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator)); FAILED(hr))
            return dxgi::failure("CreateCommandAllocator", hr);
        Buffer constants;
        if (std::string e = makeBuffer(m_ctx, kFrameConstantsBytes, D3D12_HEAP_TYPE_UPLOAD, &constants); !e.empty())
            return e;
        s.frameConstants = constants.resource;
        s.mapped[0] = constants.mapped;
        std::memset(s.mapped[0], 0, std::size_t(kFrameConstantsBytes));
    }
    if (const HRESULT hr = d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_slots[0].allocator.Get(), nullptr, IID_PPV_ARGS(&m_list));
        FAILED(hr))
        return dxgi::failure("CreateCommandList", hr);
    m_list->Close();

    // §5.6: two timestamps per slot, resolved into a READBACK buffer read after the slot's fence.
    if (m_ctx.timestampFrequency > 0) {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 2 * UINT(m_slots.size());
        if (const HRESULT hr = d->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_queries)); FAILED(hr))
            return dxgi::failure("CreateQueryHeap", hr);
        Buffer readback;
        if (std::string e = makeBuffer(m_ctx, sizeof(UINT64) * qd.Count, D3D12_HEAP_TYPE_READBACK, &readback); !e.empty())
            return e;
        m_queryReadback = readback.resource;
        m_timestamps = static_cast<UINT64 *>(readback.mapped);
    }

    // The present pipeline for the likely target format; resume() remakes it if the target differs.
    if (std::string e = makePresentPipeline(m_headless ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM, &m_presentPipeline); !e.empty())
        return e;
    m_presentFormat = m_headless ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    // The present pass into the RGBA8 readback target of the tone mapped scenes (§5.7, task 19).
    if (std::string e = makePresentPipeline(DXGI_FORMAT_R8G8B8A8_UNORM, &m_readPipeline); !e.empty())
        return e;
    return m_ctx.validationProblem();
}

std::string D3D12Backend::makePresentPipeline(DXGI_FORMAT format, ComPtr<ID3D12PipelineState> *out)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_root.Get();
    pd.VS = {kPresentVsDxil, kPresentVsDxilSize};
    pd.PS = {kPresentPsDxil, kPresentPsDxilSize};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = format;
    pd.SampleDesc.Count = 1;
    out->Reset();
    if (const HRESULT hr = m_ctx.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(out->GetAddressOf())); FAILED(hr))
        return dxgi::failure("CreateGraphicsPipelineState (present)", hr);
    return {};
}

std::string D3D12Backend::load(const SceneData &scene)
{
    if (m_loaded)
        unload();
    m_rtvNext = kRtvLoadBase;
    m_dsvNext = kDsvLoadBase;
    m_srvNext = kSrvLoadBase;
    m_work = std::make_unique<WorkloadState>();
    m_work->workload = scene.workload;
    // The render targets (§5.3, D12: load() .. unload()): ldrA always (the clear
    // workload's target, Particles' tone map output); the scenes add theirs.
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::memcpy(clear.Color, kClearColour, sizeof kClearColour);
    if (std::string e = makeImage(m_ctx, m_init.renderWidth, m_init.renderHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 1, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, &m_ldrA);
        !e.empty())
        return e;
    m_ctx.device->CreateRenderTargetView(m_ldrA.resource.Get(), nullptr, m_ctx.rtv(kRtvLdrA));
    m_ldrA.rtv = kRtvLdrA;
    m_loaded = true;
    m_lastDraws = 0;
    // Task 19: the scenes (Quads and Sprites still clear ldrA, task 30).
    std::string e;
    switch (scene.workload) {
    case Workload::Geometry:
    case Workload::DrawCalls: e = loadScene(scene); break;
    case Workload::Shading: e = loadShading(scene); break;
    case Workload::Particles: e = loadParticles(scene); break;
    case Workload::Quads:
    case Workload::Sprites: e = loadFill(scene); break;
    case Workload::Shapes:
    case Workload::Text:
    case Workload::Blits:
    case Workload::Gradients: break;
    }
    if (!e.empty())
        return e;
    // The scenes' per-slot object constants: one 256-byte slot per instance, an
    // UPLOAD buffer persistently mapped (DX-4), a root CBV per draw (task 19).
    if (!m_work->scene.instances.empty()) {
        for (Slot &s : m_slots) {
            Buffer objects;
            if (std::string oe = makeBuffer(m_ctx, UINT64(m_work->scene.instances.size()) * sc::kObjectSlot, D3D12_HEAP_TYPE_UPLOAD, &objects);
                !oe.empty())
                return oe;
            s.objectConstants = objects.resource;
            s.mapped[1] = objects.mapped;
        }
    }
    // The present pass's table: t0 = the workload's output, t1-t4 null, t5 = the atlas.
    m_presentTable = kPresentTableIndex;
    writeSrv(m_ctx, m_presentTable + 0, &outputImage());
    for (UINT i = 1; i < 5; ++i)
        writeSrv(m_ctx, m_presentTable + i, nullptr);
    writeSrv(m_ctx, m_presentTable + 5, &m_atlas);
    return m_ctx.validationProblem();
}

void D3D12Backend::unload()
{
    if (!m_ctx.device)
        return;
    waitAll();
    for (Slot &s : m_slots) {
        s.submitted = false;  // after the wait: their timestamps belong to the old workload
        s.objectConstants.Reset();  // made in load() (DX-2: after the wait)
        s.mapped[1] = nullptr;
        s.spriteRing.Reset();       // task 30: the sprites' instance ring, made in load()
        s.mapped[2] = nullptr;
    }
    unloadScene();
    m_work.reset();
    for (Image *img : {&m_ldrA, &m_sceneColor, &m_sceneDepth, &m_hdrB, &m_ldrB, &m_shadow})
        *img = Image{};
    for (Image &img : m_bloom)
        img = Image{};
    m_loaded = false;
    m_lastDraws = 0;
}

void D3D12Backend::releaseWindow()
{
    for (ComPtr<ID3D12Resource> &b : m_backBuffers)
        b.Reset();
    m_swap.destroy();
    m_headlessTarget = Image{};
    m_resumed = false;
}

std::string D3D12Backend::suspend()
{
    if (!m_ctx.device)
        return {};
    // Revised (Dave): wait for the fence, release the back buffers and the swapchain.
    std::string e = waitAll();
    for (Slot &s : m_slots)
        s.submitted = false;
    releaseWindow();
    if (!e.empty())
        return e;
    return m_ctx.validationProblem();
}

std::string D3D12Backend::resume(const PresentTarget &target)
{
    if (!m_ctx.device)
        return "resume() before init()";
    if (m_resumed)
        releaseWindow();
    m_windowScale = target.scale > 0 ? target.scale : 1.0;
    m_presentModeName.clear();
    const auto fail = [this](std::string e) {
        releaseWindow();
        return e;
    };
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (m_headless) {
        if (std::string e = makeImage(m_ctx, m_init.renderWidth, m_init.renderHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 1, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, &m_headlessTarget);
            !e.empty())
            return fail(e);
        m_ctx.device->CreateRenderTargetView(m_headlessTarget.resource.Get(), nullptr, m_ctx.rtv(kRtvHeadless));
    } else {
        if (!target.hwnd)
            return fail("the test window has no native window handle");
        std::string created;
#if defined(SYNBENCH_DEV_HOOKS)
        // Dev hook (as Vulkan's): "1" = no swapchain can be made; "later" = none after the first resume.
        if (const char *hook = std::getenv("SYNBENCH_GPU_FAIL_SWAPCHAIN"); hook && (hook[0] == '1' || (std::strcmp(hook, "later") == 0 && m_everResumed)))
            created = "N/A: this GPU can't present to the test window on this display system";
#endif
        if (created.empty())
            created = m_swap.create(m_ctx.queue.Get(), static_cast<HWND>(target.hwnd), target.windowWidth, target.windowHeight);
        if (!created.empty()) {
            // N/A only on an API's first resume (nothing was ever presented on it): after that it is an Error (L8).
            if (m_everResumed && created.rfind("N/A: ", 0) == 0)
                created = "the swapchain couldn't be made again after the first workload (" + created.substr(5) + ")";
            return fail(created);
        }
        for (UINT i = 0; i < dxgi::Swapchain::kBuffers; ++i) {
            if (const HRESULT hr = m_swap.swapchain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])); FAILED(hr))
                return fail(dxgi::failure("IDXGISwapChain::GetBuffer", hr));
            m_ctx.device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, m_ctx.rtv(kRtvBackBuffer + i));
        }
        format = DXGI_FORMAT_B8G8R8A8_UNORM;
        m_presentModeName = m_swap.modeName();
    }
    if (format != m_presentFormat) {
        if (std::string e = makePresentPipeline(format, &m_presentPipeline); !e.empty())
            return fail(e);
        m_presentFormat = format;
    }
    m_resumed = true;
    m_everResumed = true;
    if (std::string e = m_ctx.validationProblem(); !e.empty())
        return fail(e);
    return {};
}

std::string D3D12Backend::waitSlot(std::uint32_t index, std::vector<double> *gpuMs)
{
    Slot &s = m_slots[index];
    if (!s.submitted)
        return {};
    // DX-2: the slot's allocator and constants are reused only after its fence value.
    if (std::string e = m_ctx.wait(s.fenceValue); !e.empty())
        return e;
    s.submitted = false;
    if (gpuMs && m_timestamps) {
        const UINT64 begin = m_timestamps[2 * index], end = m_timestamps[2 * index + 1];
        if (end >= begin)
            gpuMs->push_back(double(end - begin) * 1000.0 / double(m_ctx.timestampFrequency));
    }
    return {};
}

std::string D3D12Backend::waitAll()
{
    for (std::uint32_t k = 0; k < m_slots.size(); ++k) {
        if (std::string e = waitSlot(std::uint32_t((m_frame + k) % m_slots.size()), nullptr); !e.empty())
            return e;
    }
    return {};
}

void D3D12Backend::recordWorkload(ID3D12GraphicsCommandList *list, Slot &slot, double t)
{
    // M1: valid defaults for every root parameter, so no draw of any workload runs with one unset
    // (the descriptor heap is already set by frame() / readImage()).
    list->SetGraphicsRootSignature(m_root.Get());
    const D3D12_GPU_VIRTUAL_ADDRESS frameAddress = slot.frameConstants->GetGPUVirtualAddress();
    list->SetGraphicsRootConstantBufferView(0, frameAddress);
    list->SetGraphicsRootConstantBufferView(1, slot.objectConstants ? slot.objectConstants->GetGPUVirtualAddress() : frameAddress);
    list->SetGraphicsRootConstantBufferView(2, frameAddress + kPassBase);
    list->SetGraphicsRootDescriptorTable(3, m_ctx.srvGpu(m_presentTable));
    switch (m_work ? m_work->workload : Workload::Quads) {
    case Workload::Geometry:
    case Workload::DrawCalls: recordScene(list, slot, t); return;
    case Workload::Shading: recordShading(list, slot, t); return;
    case Workload::Particles: recordParticles(list, slot, t); return;
    case Workload::Quads:
    case Workload::Sprites: recordFill(list, slot, t); return;
    case Workload::Shapes:
    case Workload::Text:
    case Workload::Blits:
    case Workload::Gradients: break;
    }
    m_lastDraws = 0;  // the clear workload draws nothing before the present pass
    // DX-1: the previous frame's present pass sampled ldrA, a readback copied it.
    transition(list, m_ldrA, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(m_ctx.rtv(kRtvLdrA), kClearColour, 0, nullptr);
}

void D3D12Backend::recordPresentPass(ID3D12GraphicsCommandList *list, Slot &slot, D3D12_CPU_DESCRIPTOR_HANDLE rtv, std::uint32_t w,
                                     std::uint32_t h, ID3D12PipelineState *pipeline)
{
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    // D3D's viewport is y-down already: the D3D-convention matrices need no flip (VK-3's counterpart).
    const D3D12_VIEWPORT vp{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(m_root.Get());
    list->SetPipelineState(pipeline);
    const D3D12_GPU_VIRTUAL_ADDRESS constants = slot.frameConstants->GetGPUVirtualAddress();
    list->SetGraphicsRootConstantBufferView(0, constants);
    list->SetGraphicsRootConstantBufferView(1, constants);  // b1 is unused by the present pass; a valid address all the same
    list->SetGraphicsRootConstantBufferView(2, constants + kPassBase);
    list->SetGraphicsRootDescriptorTable(3, m_ctx.srvGpu(m_presentTable));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
}

std::string D3D12Backend::frame(double t, std::vector<double> *gpuMs)
{
    if (!m_loaded || !m_resumed)
        return "frame() before load() and resume()";
    const std::uint32_t si = std::uint32_t(m_frame % m_slots.size());
    Slot &s = m_slots[si];
    // §5.4: frame n reuses frame n-2's slot only after its fence value (DX-2),
    // and collects that frame's GPU time (P8).
    if (std::string e = waitSlot(si, gpuMs); !e.empty())
        return e;
    const std::uint32_t outW = m_headless ? m_headlessTarget.width : m_swap.width;
    const std::uint32_t outH = m_headless ? m_headlessTarget.height : m_swap.height;
    const PresentConstants pc = presentConstants(tonemapped() ? kPresentTonemap : kPresentCopy, m_overlay, m_windowScale, outW, outH);
    std::memcpy(static_cast<std::uint8_t *>(s.mapped[0]) + kPassBase, &pc, sizeof pc);
    if (!m_headless) {
        if (std::string e = m_swap.waitForFrame(); !e.empty())
            return e;
    }

    if (const HRESULT hr = s.allocator->Reset(); FAILED(hr))
        return dxgi::failure("ID3D12CommandAllocator::Reset", hr);
    if (const HRESULT hr = m_list->Reset(s.allocator.Get(), nullptr); FAILED(hr))
        return dxgi::failure("ID3D12GraphicsCommandList::Reset", hr);
    ID3D12GraphicsCommandList *list = m_list.Get();
    ID3D12DescriptorHeap *heaps[] = {m_ctx.srvHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    if (m_queries)
        list->EndQuery(m_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * si);

    recordWorkload(list, s, t);
    // The workload's output -> the present pass's pixel-shader reads (Vulkan: COLOR_ATTACHMENT -> SHADER_READ_ONLY).
    transition(list, outputImage(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // The present pass (P3) into the back buffer (DX-3: PRESENT -> RENDER_TARGET -> PRESENT) or the headless target.
    if (m_headless) {
        transition(list, m_headlessTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
        recordPresentPass(list, s, m_ctx.rtv(kRtvHeadless), outW, outH, m_presentPipeline.Get());
    } else {
        const UINT bb = m_swap.swapchain->GetCurrentBackBufferIndex();
        transition(list, m_backBuffers[bb].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        recordPresentPass(list, s, m_ctx.rtv(kRtvBackBuffer + bb), outW, outH, m_presentPipeline.Get());
        transition(list, m_backBuffers[bb].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }
    if (m_queries) {
        list->EndQuery(m_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * si + 1);
        list->ResolveQueryData(m_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * si, 2, m_queryReadback.Get(), sizeof(UINT64) * 2 * si);
    }
    if (const HRESULT hr = list->Close(); FAILED(hr))
        return dxgi::failure("ID3D12GraphicsCommandList::Close", hr);
    ID3D12CommandList *lists[] = {list};
    m_ctx.queue->ExecuteCommandLists(1, lists);
    if (std::string e = m_ctx.signal(&s.fenceValue); !e.empty())
        return e;
    s.submitted = true;
    ++m_frame;
    if (!m_headless) {
        if (std::string e = m_swap.present(); !e.empty())
            return e;
    }
    if (std::string lost = m_ctx.removedReason(); !lost.empty())
        return lost;
    return m_ctx.validationProblem();
}

std::string D3D12Backend::drain(std::vector<double> *gpuMs)
{
    // The oldest pending frame first, so gpuMs stays in submission order.
    for (std::uint32_t k = 0; k < m_slots.size(); ++k) {
        if (std::string e = waitSlot(std::uint32_t((m_frame + k) % m_slots.size()), gpuMs); !e.empty())
            return e;
    }
    return m_ctx.validationProblem();
}

std::string D3D12Backend::readImage(Image &img, const std::function<void(ID3D12GraphicsCommandList *)> &pre, std::vector<std::uint8_t> *rgba8)
{
    if (std::string e = waitAll(); !e.empty())
        return e;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    const UINT64 bytes = readbackBytes(m_ctx, img, &footprint);
    Buffer host;
    if (std::string e = makeBuffer(m_ctx, bytes, D3D12_HEAP_TYPE_READBACK, &host); !e.empty())
        return e;
    std::string e = m_ctx.oneTimeSubmit([&](ID3D12GraphicsCommandList *list) {
        if (pre) {
            ID3D12DescriptorHeap *heaps[] = {m_ctx.srvHeap.Get()};
            list->SetDescriptorHeaps(1, heaps);
            pre(list);
        }
        recordReadback(list, img, host, footprint);
    });
    if (!e.empty())
        return e;
    unpack(host, footprint, img.width, img.height, rgba8);  // RGBA8_UNORM, row 0 = the top
    return m_ctx.validationProblem();
}

std::string D3D12Backend::readback(double t, std::vector<std::uint8_t> *rgba8)
{
    if (!m_loaded)
        return "readback() before load()";
    // §5.7: the workload's output before the present pass, exactly render size.
    // Slot 0 supplies the constants (readImage waits for every slot first).
    Slot &s = m_slots[0];
    if (!tonemapped())
        return readImage(outputImage(), [&](ID3D12GraphicsCommandList *list) { recordWorkload(list, s, t); }, rgba8);
    // A tone mapped scene (sceneColor is RGBA16F, linear): the present pass in
    // mode 0, no overlay, into an RGBA8 target at render size (task 11's rule).
    if (std::string e = waitAll(); !e.empty())
        return e;
    Image &target = m_work->readTarget;
    const PresentConstants pc = presentConstants(kPresentTonemap, std::string(), 1.0, target.width, target.height);
    std::memcpy(static_cast<std::uint8_t *>(s.mapped[0]) + kPassBase, &pc, sizeof pc);
    return readImage(
        target,
        [&](ID3D12GraphicsCommandList *list) {
            recordWorkload(list, s, t);
            transition(list, m_sceneColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET);
            recordPresentPass(list, s, m_ctx.rtv(target.rtv), target.width, target.height, m_readPipeline.Get());
        },
        rgba8);
}

#if defined(SYNBENCH_DEV_HOOKS)
std::string D3D12Backend::readbackPresented(std::vector<std::uint8_t> *rgba8)
{
    if (!m_headless || !m_resumed || m_frame == 0)
        return "readbackPresented() needs a headless frame";
    return readImage(m_headlessTarget, nullptr, rgba8);
}
#endif

} // namespace sb::gpu::d3d12

namespace sb::gpu {
std::unique_ptr<IBackend> makeD3D12Backend()
{
    return std::make_unique<d3d12::D3D12Backend>();
}
} // namespace sb::gpu
