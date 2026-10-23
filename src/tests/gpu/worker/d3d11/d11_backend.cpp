// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/d3d11/d11_backend.h"

#include "tests/gpu/worker/d3d11/d11_workload_state.h"

#include "tests/gpu/worker/present_constants.h"
#include "tests/gpu/worker/scene/bitmap_font.h"
#include "tests/gpu/worker/scene/textures.h"
#include "tests/gpu/worker/scene_constants.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

namespace {
// The Windows build's DXBC (worker/shaders/CMakeLists.txt, SYNBENCH_GPU_SHADER_DIR).
#include "present_ps.dxbc.inc"
#include "present_vs.dxbc.inc"
} // namespace

namespace sb::gpu::d3d11 {

static_assert(kPassBase == sc::kPresentOffset && kFrameConstantsBytes == sc::kFrameConstantsBytes && kFrameBlock == sc::kBlock,
              "D3D11's frame-constants layout is scene_constants.h's (and Vulkan's)");

namespace {

// The clear-only workload (task 10's, §5.3 Copy mode): ldrA = (64, 128, 192, 255).
constexpr float kClearColour[4] = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};

// The UMD version DXGI reports (CheckInterfaceSupport), as "31.0.15.5222".
std::string driverVersion(IDXGIAdapter1 *adapter)
{
    LARGE_INTEGER umd{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        return {};
    char text[64];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", unsigned(HIWORD(umd.HighPart)), unsigned(LOWORD(umd.HighPart)),
                  unsigned(HIWORD(umd.LowPart)), unsigned(LOWORD(umd.LowPart)));
    return text;
}

D3D11_SAMPLER_DESC samplerDesc(D3D11_FILTER filter, D3D11_TEXTURE_ADDRESS_MODE address)
{
    D3D11_SAMPLER_DESC s{};
    s.Filter = filter;
    s.AddressU = s.AddressV = s.AddressW = address;
    s.MipLODBias = 0.0f;
    s.MaxAnisotropy = 1;
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    s.BorderColor[0] = s.BorderColor[1] = s.BorderColor[2] = s.BorderColor[3] = 1.0f;  // opaque white (Vulkan's s2)
    s.MinLOD = 0.0f;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    return s;
}

} // namespace

std::string makePipeline(ID3D11Device *d, const PipelineDesc &p, Pipeline *out)
{
    Pipeline r;
    if (const HRESULT hr = d->CreateVertexShader(p.vs, p.vsSize, nullptr, &r.vs); FAILED(hr))
        return dxgi::failure("CreateVertexShader", hr);
    if (p.ps) {
        if (const HRESULT hr = d->CreatePixelShader(p.ps, p.psSize, nullptr, &r.ps); FAILED(hr))
            return dxgi::failure("CreatePixelShader", hr);
    }
    // The Vertex layout (32 bytes) by the HLSL semantics; Particle's two float4 per instance.
    const D3D11_INPUT_ELEMENT_DESC mesh[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, px), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, nx), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    // §6.6: 16 bytes per sprite, one instance each: {float x, y; uint packed; uint rgba8}.
    const D3D11_INPUT_ELEMENT_DESC sprite[] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 12, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    const D3D11_INPUT_ELEMENT_DESC particle[] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    if (p.input != PipelineDesc::Input::None) {
        const bool isMesh = p.input == PipelineDesc::Input::Mesh, isSprite = p.input == PipelineDesc::Input::Sprite;
        if (const HRESULT hr = d->CreateInputLayout(isMesh ? mesh : isSprite ? sprite : particle, isMesh || isSprite ? 3 : 2, p.vs, p.vsSize, &r.layout);
            FAILED(hr))
            return dxgi::failure("CreateInputLayout", hr);
    }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // task 11: both faces on every API
    rd.FrontCounterClockwise = FALSE;
    rd.DepthBias = 0;
    rd.DepthBiasClamp = 0.0f;
    rd.SlopeScaledDepthBias = p.depthBias ? 2.0f : 0.0f;
    rd.DepthClipEnable = TRUE;
    if (const HRESULT hr = d->CreateRasterizerState(&rd, &r.raster); FAILED(hr))
        return dxgi::failure("CreateRasterizerState", hr);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = p.depthTest ? TRUE : FALSE;
    dd.DepthWriteMask = p.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = p.compare;
    if (const HRESULT hr = d->CreateDepthStencilState(&dd, &r.depth); FAILED(hr))
        return dxgi::failure("CreateDepthStencilState", hr);
    D3D11_BLEND_DESC bd{};
    D3D11_RENDER_TARGET_BLEND_DESC &rt = bd.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (p.additive) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ONE;
        rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    } else if (p.alphaBlend) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    if (const HRESULT hr = d->CreateBlendState(&bd, &r.blend); FAILED(hr))
        return dxgi::failure("CreateBlendState", hr);
    *out = std::move(r);
    return {};
}

D3D11Backend::D3D11Backend() = default;

D3D11Backend::~D3D11Backend()
{
    shutdown();
}

void D3D11Backend::shutdown()
{
    if (m_device) {
        waitAll();
        unload();
        releaseWindow();
    }
    for (QuerySlot &q : m_queries)
        q = QuerySlot{};
    m_presentPipeline = Pipeline{};
    m_presentTable = Table{};
    m_atlas = Image{};
    m_frameCb.Reset();
    for (ComPtr<ID3D11SamplerState> &s : m_samplers)
        s.Reset();
    if (m_cx) {
        m_cx->ClearState();
        m_cx->Flush();
    }
    m_cx.Reset();
    m_device.Reset();
    m_info.Reset();
    m_adapter.Reset();
}

std::string D3D11Backend::createDevice(const BackendInit &init, BackendInfo *info)
{
    if (std::string e = dxgi::findAdapter(init, &m_adapter, init.allowSoftware); !e.empty())
        return e;
    DXGI_ADAPTER_DESC1 desc{};
    m_adapter->GetDesc1(&desc);
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    UINT flags = init.validation ? D3D11_CREATE_DEVICE_DEBUG : 0;
    HRESULT hr = D3D11CreateDevice(m_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device, &level, &context);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        // --validation: the debug layer when it is installed (Windows' "Graphics Tools").
        std::fprintf(stderr, "synbench-gpu: d3d11: the debug layer isn't installed; running without validation\n");
        flags = 0;
        hr = D3D11CreateDevice(m_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device, &level, &context);
    }
    if (FAILED(hr))
        return "N/A: Needs DirectX 11 feature level 11_0";
    if (FAILED(device.As(&m_device)) || FAILED(context.As(&m_cx)))
        return "N/A: Needs DirectX 11.1 constant-buffer offsets";
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (FAILED(m_device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof options)) || !options.ConstantBufferOffsetting)
        return "N/A: Needs DirectX 11.1 constant-buffer offsets";
    if (flags & D3D11_CREATE_DEVICE_DEBUG) {
        if (SUCCEEDED(m_device.As(&m_info))) {
            m_info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            m_info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
        }
    }
    // §5.4: at most 2 frames queued by the runtime (the swapchain's waitable object does the same for a window).
    ComPtr<IDXGIDevice1> dxgiDevice;
    if (SUCCEEDED(m_device.As(&dxgiDevice)))
        dxgiDevice->SetMaximumFrameLatency(2);
    if (info) {
        info->deviceName = dxgi::utf8(desc.Description);
        info->apiVersion = std::string("DirectX 11 (feature level ") + (level == D3D_FEATURE_LEVEL_11_1 ? "11_1" : "11_0") + ")";
        info->driver = driverVersion(m_adapter.Get());
        info->vendorId = desc.VendorId;
        info->deviceId = desc.DeviceId;
        info->layers.clear();
        info->presentMode.clear();  // known after resume() (the result's "present")
    }
    return {};
}

std::string D3D11Backend::init(const BackendInit &init, const PresentTarget &target, BackendInfo *info)
{
    m_init = init;
    m_headless = target.headless;
    if (std::string e = createDevice(init, info); !e.empty())
        return e;
    ID3D11Device *d = m_device.Get();

    // s0 anisotropic 8 wrap, s1 linear clamp, s2 comparison LESS_EQUAL (border white, one mip), s3 plain bilinear wrap
    // (no mips, no anisotropy: the Quads, §6.5).
    D3D11_SAMPLER_DESC s0 = samplerDesc(D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP);
    s0.MaxAnisotropy = 8;
    const D3D11_SAMPLER_DESC s1 = samplerDesc(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP);
    D3D11_SAMPLER_DESC s2 = samplerDesc(D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D11_TEXTURE_ADDRESS_BORDER);
    s2.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    s2.MaxLOD = 0.0f;
    D3D11_SAMPLER_DESC s3 = samplerDesc(D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP);
    s3.MaxLOD = 0.0f;
    const D3D11_SAMPLER_DESC descs[4] = {s0, s1, s2, s3};
    for (std::size_t i = 0; i < 4; ++i) {
        if (const HRESULT hr = d->CreateSamplerState(&descs[i], &m_samplers[i]); FAILED(hr))
            return dxgi::failure("CreateSamplerState", hr);
    }

    // The glyph atlas, once (§5.9).
    if (std::string e = uploadTexture(glyphAtlas(), &m_atlas); !e.empty())
        return e;

    // The frame constants: a DYNAMIC buffer (Map(WRITE_DISCARD) once per frame) with a CPU copy.
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kFrameConstantsBytes;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (const HRESULT hr = d->CreateBuffer(&bd, nullptr, &m_frameCb); FAILED(hr))
        return dxgi::failure("CreateBuffer (frame constants)", hr);
    m_frameShadow.assign(kFrameConstantsBytes, 0);

    // §5.6: the disjoint query and two timestamps per slot; none -> GPU time "-".
    m_timestamps = true;
    for (QuerySlot &q : m_queries) {
        const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(d->CreateQuery(&disjoint, &q.disjoint)) || FAILED(d->CreateQuery(&stamp, &q.begin)) || FAILED(d->CreateQuery(&stamp, &q.end)))
            m_timestamps = false;
    }
    if (info)
        info->gpuTimestamps = m_timestamps;

    PipelineDesc present;
    present.vs = kPresentVsDxbc;
    present.vsSize = kPresentVsDxbcSize;
    present.ps = kPresentPsDxbc;
    present.psSize = kPresentPsDxbcSize;
    if (std::string e = makePipeline(d, present, &m_presentPipeline); !e.empty())
        return e;
    return validationProblem();
}

std::string D3D11Backend::validationProblem()
{
    if (m_info) {
        const UINT64 n = m_info->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(m_info->GetMessage(i, nullptr, &bytes)) || bytes == 0)
                continue;
            std::vector<unsigned char> buffer(bytes);
            auto *m = reinterpret_cast<D3D11_MESSAGE *>(buffer.data());
            if (FAILED(m_info->GetMessage(i, m, &bytes)))
                continue;
            // DX11-1: the runtime only warns when it silently unbinds a hazard, so those count as errors.
            const bool hazard = m->ID == D3D11_MESSAGE_ID_DEVICE_OMSETRENDERTARGETS_HAZARD || m->ID == D3D11_MESSAGE_ID_DEVICE_PSSETSHADERRESOURCES_HAZARD
                                || m->ID == D3D11_MESSAGE_ID_DEVICE_VSSETSHADERRESOURCES_HAZARD;
            if (m->Severity != D3D11_MESSAGE_SEVERITY_ERROR && m->Severity != D3D11_MESSAGE_SEVERITY_CORRUPTION && !hazard)
                continue;
            const std::string text(m->pDescription ? m->pDescription : "(no message)");
            std::fprintf(stderr, "synbench-gpu: d3d11: %s\n", text.c_str());
            if (m_validationErrors++ == 0)
                m_firstValidationError = text;
        }
        m_info->ClearStoredMessages();
    }
    if (m_validationErrors == 0)
        return {};
    return "validation: " + m_firstValidationError;
}

std::string D3D11Backend::removedReason()
{
    if (m_device && FAILED(m_device->GetDeviceRemovedReason()))
        return "LOST: the GPU driver reset (DXGI_ERROR_DEVICE_REMOVED)";
    return {};
}

std::string D3D11Backend::uploadBuffer(const void *data, std::size_t bytes, UINT bindFlags, ComPtr<ID3D11Buffer> *out)
{
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = UINT(bytes);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = bindFlags;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = data;
    if (const HRESULT hr = m_device->CreateBuffer(&bd, &sd, out->ReleaseAndGetAddressOf()); FAILED(hr))
        return dxgi::failure("CreateBuffer", hr);
    return {};
}

std::string D3D11Backend::uploadTexture(const Texture &level0, Image *out)
{
    const std::vector<Texture> chain = level0.mips ? mipChain(level0) : std::vector<Texture>{level0};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = level0.width;
    td.Height = level0.height;
    td.MipLevels = UINT(chain.size());
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> data(chain.size());
    for (std::size_t i = 0; i < chain.size(); ++i) {
        data[i].pSysMem = chain[i].rgba8.data();
        data[i].SysMemPitch = chain[i].width * 4;
    }
    Image img;
    img.width = level0.width;
    img.height = level0.height;
    if (const HRESULT hr = m_device->CreateTexture2D(&td, data.data(), &img.texture); FAILED(hr))
        return dxgi::failure("CreateTexture2D", hr);
    if (const HRESULT hr = m_device->CreateShaderResourceView(img.texture.Get(), nullptr, &img.srv); FAILED(hr))
        return dxgi::failure("CreateShaderResourceView", hr);
    *out = std::move(img);
    return {};
}

std::string D3D11Backend::makeTarget(Image &img, std::uint32_t w, std::uint32_t h, DXGI_FORMAT format, Kind kind)
{
    Image out;
    out.width = w;
    out.height = h;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = kind == Kind::Colour        ? (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)
                   : kind == Kind::Depth       ? D3D11_BIND_DEPTH_STENCIL
                                               : (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    if (const HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &out.texture); FAILED(hr))
        return dxgi::failure("CreateTexture2D (target)", hr);
    if (kind == Kind::Colour) {
        if (const HRESULT hr = m_device->CreateRenderTargetView(out.texture.Get(), nullptr, &out.rtv); FAILED(hr))
            return dxgi::failure("CreateRenderTargetView", hr);
        if (const HRESULT hr = m_device->CreateShaderResourceView(out.texture.Get(), nullptr, &out.srv); FAILED(hr))
            return dxgi::failure("CreateShaderResourceView", hr);
    } else {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D32_FLOAT;  // also the R32_TYPELESS shadow map's
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (const HRESULT hr = m_device->CreateDepthStencilView(out.texture.Get(), &dd, &out.dsv); FAILED(hr))
            return dxgi::failure("CreateDepthStencilView", hr);
        if (kind == Kind::SampledDepth) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32_FLOAT;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MostDetailedMip = 0;
            sd.Texture2D.MipLevels = 1;
            if (const HRESULT hr = m_device->CreateShaderResourceView(out.texture.Get(), &sd, &out.srv); FAILED(hr))
                return dxgi::failure("CreateShaderResourceView (shadow map)", hr);
        }
    }
    img = std::move(out);
    return {};
}

Table D3D11Backend::makeTable(std::initializer_list<const Image *> images)
{
    Table t;
    std::size_t i = 0;
    for (const Image *img : images)
        t[i++] = img->srv;
    return t;
}

std::string D3D11Backend::load(const SceneData &scene)
{
    if (m_loaded)
        unload();
    m_work = std::make_unique<WorkloadState>();
    m_work->workload = scene.workload;
    // The render targets (§5.3, D12: load() .. unload()): ldrA always (the clear
    // workload's target, Particles' tone map output); the scenes add theirs.
    if (std::string e = makeTarget(m_ldrA, m_init.renderWidth, m_init.renderHeight, DXGI_FORMAT_R8G8B8A8_UNORM, Kind::Colour); !e.empty())
        return e;
    m_loaded = true;
    m_lastDraws = 0;
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
    // The scenes' object constants: one 256-byte slot per instance, a DYNAMIC
    // buffer written with Map(WRITE_DISCARD) once per frame (DX11-2).
    if (!m_work->scene.instances.empty()) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(m_work->scene.instances.size() * sc::kObjectSlot);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (const HRESULT hr = m_device->CreateBuffer(&bd, nullptr, &m_objectCb); FAILED(hr))
            return dxgi::failure("CreateBuffer (object constants)", hr);
    }
    // The present pass's table: t0 = the workload's output, t1-t4 null, t5 = the atlas.
    m_presentTable = Table{};
    m_presentTable[0] = outputImage().srv;
    m_presentTable[5] = m_atlas.srv;
    return validationProblem();
}

void D3D11Backend::unload()
{
    if (!m_device)
        return;
    waitAll();  // after it: the pending frames' timestamps belong to the old workload
    if (m_cx) {
        m_cx->ClearState();  // nothing bound still references the workload's resources
        m_cx->Flush();
    }
    m_work.reset();
    m_objectCb.Reset();
    m_spriteCb.Reset();
    for (Image *img : {&m_ldrA, &m_sceneColor, &m_sceneDepth, &m_hdrB, &m_ldrB, &m_shadow})
        *img = Image{};
    for (Image &img : m_bloom)
        img = Image{};
    m_presentTable = Table{};
    m_loaded = false;
    m_lastDraws = 0;
}

void D3D11Backend::releaseWindow()
{
    // The order Microsoft requires for a flip-model swapchain (H1): the views, the back buffer and the
    // swapchain are released FIRST, then ClearState and Flush let the runtime drop its last references.
    // Otherwise the next CreateSwapChainForHwnd on the same HWND can fail with E_ACCESSDENIED.
    m_backRtv.Reset();
    m_backTexture.Reset();
    m_swap.destroy();
    if (m_cx) {
        m_cx->ClearState();
        m_cx->Flush();
    }
    m_headlessTarget = Image{};
    m_resumed = false;
}

std::string D3D11Backend::suspend()
{
    if (!m_device)
        return {};
    std::string e = waitAll();
    releaseWindow();
    if (!e.empty())
        return e;
    return validationProblem();
}

std::string D3D11Backend::resume(const PresentTarget &target)
{
    if (!m_device)
        return "resume() before init()";
    if (m_resumed)
        releaseWindow();
    m_windowScale = target.scale > 0 ? target.scale : 1.0;
    m_presentModeName.clear();
    const auto fail = [this](std::string e) {
        releaseWindow();
        return e;
    };
    if (m_headless) {
        if (std::string e = makeTarget(m_headlessTarget, m_init.renderWidth, m_init.renderHeight, DXGI_FORMAT_R8G8B8A8_UNORM, Kind::Colour); !e.empty())
            return fail(e);
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
            created = m_swap.create(m_device.Get(), static_cast<HWND>(target.hwnd), target.windowWidth, target.windowHeight);
        if (!created.empty()) {
            // N/A only on an API's first resume (nothing was ever presented on it): after that it is an Error (L8).
            if (m_everResumed && created.rfind("N/A: ", 0) == 0)
                created = "the swapchain couldn't be made again after the first workload (" + created.substr(5) + ")";
            return fail(created);
        }
        // A flip-model swapchain's buffer 0 is the one to draw into; the runtime rotates the buffers.
        if (const HRESULT hr = m_swap.swapchain->GetBuffer(0, IID_PPV_ARGS(&m_backTexture)); FAILED(hr))
            return fail(dxgi::failure("IDXGISwapChain::GetBuffer", hr));
        if (const HRESULT hr = m_device->CreateRenderTargetView(m_backTexture.Get(), nullptr, &m_backRtv); FAILED(hr))
            return fail(dxgi::failure("CreateRenderTargetView (back buffer)", hr));
        m_presentModeName = m_swap.modeName();
    }
    m_resumed = true;
    m_everResumed = true;
    if (std::string e = validationProblem(); !e.empty())
        return fail(e);
    return {};
}

std::string D3D11Backend::waitSlot(std::uint32_t index, std::vector<double> *gpuMs)
{
    QuerySlot &q = m_queries[index];
    if (!q.pending)
        return {};
    q.pending = false;
    // §5.4/§5.6: the frame's results (the disjoint query ends the frame) are waited for before its
    // slot is reused, so at most 2 frames are in flight; GetData with DONOTFLUSH after one Flush.
    const auto poll = [this](ID3D11Query *query, void *data, UINT size) -> std::string {
        m_cx->Flush();
        const auto start = std::chrono::steady_clock::now();
        unsigned spins = 0;
        for (;;) {
            const HRESULT hr = m_cx->GetData(query, data, size, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_OK)
                return {};
            if (FAILED(hr))
                return dxgi::failure("GetData", hr);
            if (std::string lost = removedReason(); !lost.empty())
                return lost;
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10))  // P-STOP
                return "LOST: the GPU stopped responding";
            // L1: a short back-off, not a hot spin: a few pauses, then the thread's quantum, then 1 ms naps.
            if (++spins < 32)
                YieldProcessor();
            else if (spins < 4096)
            {
                if (!SwitchToThread())
                    Sleep(0);
            }
            else
                Sleep(1);
        }
    };
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    if (std::string e = poll(q.disjoint.Get(), &disjoint, sizeof disjoint); !e.empty())
        return e;
    if (gpuMs && !disjoint.Disjoint && disjoint.Frequency > 0) {  // a disjoint frame is dropped
        UINT64 begin = 0, end = 0;
        if (std::string e = poll(q.begin.Get(), &begin, sizeof begin); !e.empty())
            return e;
        if (std::string e = poll(q.end.Get(), &end, sizeof end); !e.empty())
            return e;
        if (end >= begin)
            gpuMs->push_back(double(end - begin) * 1000.0 / double(disjoint.Frequency));
    }
    return {};
}

std::string D3D11Backend::waitAll()
{
    for (std::uint32_t k = 0; k < m_queries.size(); ++k) {
        if (std::string e = waitSlot(std::uint32_t((m_frame + k) % m_queries.size()), nullptr); !e.empty())
            return e;
    }
    return {};
}

void D3D11Backend::bindCommon()
{
    ID3D11SamplerState *samplers[4] = {m_samplers[0].Get(), m_samplers[1].Get(), m_samplers[2].Get(), m_samplers[3].Get()};
    m_cx->VSSetSamplers(0, 4, samplers);
    m_cx->PSSetSamplers(0, 4, samplers);
    m_cx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    setConstants(0, m_frameCb.Get(), 0, kFrameBlock);
}

void D3D11Backend::bindPipeline(const Pipeline &p)
{
    m_cx->VSSetShader(p.vs.Get(), nullptr, 0);
    m_cx->PSSetShader(p.ps.Get(), nullptr, 0);
    m_cx->IASetInputLayout(p.layout.Get());
    m_cx->RSSetState(p.raster.Get());
    m_cx->OMSetDepthStencilState(p.depth.Get(), 0);
    m_cx->OMSetBlendState(p.blend.Get(), nullptr, 0xFFFFFFFFu);
    m_psBound = p.ps != nullptr;
}

void D3D11Backend::setConstants(UINT slot, ID3D11Buffer *buffer, UINT byteOffset, UINT bytes, bool ps)
{
    // The *1 calls count in constants (16 bytes), the offset a multiple of 16 constants (256 B), the count too.
    const UINT first = byteOffset / 16, count = bytes / 16;
    m_cx->VSSetConstantBuffers1(slot, 1, &buffer, &first, &count);
    if (ps)
        m_cx->PSSetConstantBuffers1(slot, 1, &buffer, &first, &count);
}

void D3D11Backend::bindTable(const Table &t)
{
    ID3D11ShaderResourceView *views[6];
    for (std::size_t i = 0; i < 6; ++i)
        views[i] = t[i].Get();
    m_cx->PSSetShaderResources(0, 6, views);
}

void D3D11Backend::unbindSrvs()
{
    ID3D11ShaderResourceView *none[6] = {};
    m_cx->PSSetShaderResources(0, 6, none);
}

void D3D11Backend::setViewport(std::uint32_t w, std::uint32_t h)
{
    // D3D's viewport is y-down, the same window mapping as Vulkan's negative-height one (VK-3).
    const D3D11_VIEWPORT vp{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
    m_cx->RSSetViewports(1, &vp);
}

std::string D3D11Backend::flushFrameConstants()
{
    D3D11_MAPPED_SUBRESOURCE m{};
    if (const HRESULT hr = m_cx->Map(m_frameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m); FAILED(hr))
        return dxgi::failure("Map (frame constants)", hr);
    std::memcpy(m.pData, m_frameShadow.data(), m_frameShadow.size());
    m_cx->Unmap(m_frameCb.Get(), 0);
    return {};
}

std::string D3D11Backend::recordWorkload(double t)
{
    bindCommon();
    // No workload loaded (unreachable today): the clear workload below, never a fill path without its state.
    if (m_work) {
        switch (m_work->workload) {
        case Workload::Geometry:
        case Workload::DrawCalls: return recordScene(t);
        case Workload::Shading: return recordShading(t);
        case Workload::Particles: return recordParticles(t);
        case Workload::Quads:
        case Workload::Sprites: return recordFill(t);
        case Workload::Shapes:
        case Workload::Text:
        case Workload::Blits:
        case Workload::Gradients: break;
        }
    }
    m_lastDraws = 0;  // the clear workload draws nothing before the present pass
    if (std::string e = flushFrameConstants(); !e.empty())
        return e;
    unbindSrvs();  // DX11-1: the previous frame's present pass sampled ldrA
    ID3D11RenderTargetView *rtv = m_ldrA.rtv.Get();
    m_cx->OMSetRenderTargets(1, &rtv, nullptr);
    m_cx->ClearRenderTargetView(rtv, kClearColour);
    return {};
}

void D3D11Backend::recordPresentPass(ID3D11RenderTargetView *rtv, std::uint32_t w, std::uint32_t h)
{
    unbindSrvs();  // DX11-1: the output was this pass's source and, a moment ago, a render target
    m_cx->OMSetRenderTargets(1, &rtv, nullptr);
    setViewport(w, h);
    bindPipeline(m_presentPipeline);
    setConstants(0, m_frameCb.Get(), 0, kFrameBlock);
    setConstants(2, m_frameCb.Get(), kPassBase, kFrameBlock);
    bindTable(m_presentTable);
    m_cx->Draw(3, 0);
}

std::string D3D11Backend::frame(double t, std::vector<double> *gpuMs)
{
    if (!m_loaded || !m_resumed)
        return "frame() before load() and resume()";
    const std::uint32_t si = std::uint32_t(m_frame % m_queries.size());
    // §5.4: frame n reuses frame n-2's slot only after its results are in, and collects that frame's GPU time (P8).
    if (std::string e = waitSlot(si, gpuMs); !e.empty())
        return e;
    const std::uint32_t outW = m_headless ? m_headlessTarget.width : m_swap.width;
    const std::uint32_t outH = m_headless ? m_headlessTarget.height : m_swap.height;
    const PresentConstants pc = presentConstants(tonemapped() ? kPresentTonemap : kPresentCopy, m_overlay, m_windowScale, outW, outH);
    std::memcpy(m_frameShadow.data() + kPassBase, &pc, sizeof pc);
    if (!m_headless) {
        if (std::string e = m_swap.waitForFrame(); !e.empty())
            return e;
    }

    QuerySlot &q = m_queries[si];
    if (m_timestamps) {
        m_cx->Begin(q.disjoint.Get());
        m_cx->End(q.begin.Get());
    }
    if (std::string e = recordWorkload(t); !e.empty())
        return e;
    // The present pass (P3) into the back buffer or the headless target.
    recordPresentPass(m_headless ? m_headlessTarget.rtv.Get() : m_backRtv.Get(), outW, outH);
    if (m_timestamps) {
        m_cx->End(q.end.Get());
        m_cx->End(q.disjoint.Get());
        q.pending = true;
    }
    ++m_frame;
    if (!m_headless) {
        if (std::string e = m_swap.present(); !e.empty())
            return e;
    }
    if (std::string lost = removedReason(); !lost.empty())
        return lost;
    return validationProblem();
}

std::string D3D11Backend::drain(std::vector<double> *gpuMs)
{
    // The oldest pending frame first, so gpuMs stays in submission order.
    for (std::uint32_t k = 0; k < m_queries.size(); ++k) {
        if (std::string e = waitSlot(std::uint32_t((m_frame + k) % m_queries.size()), gpuMs); !e.empty())
            return e;
    }
    m_cx->Flush();
    return validationProblem();
}

std::string D3D11Backend::readImage(Image &img, std::vector<std::uint8_t> *rgba8)
{
    D3D11_TEXTURE2D_DESC td{};
    img.texture->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (const HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &staging); FAILED(hr))
        return dxgi::failure("CreateTexture2D (readback)", hr);
    m_cx->CopyResource(staging.Get(), img.texture.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (const HRESULT hr = m_cx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m); FAILED(hr))
        return dxgi::failure("Map (readback)", hr);
    rgba8->resize(std::size_t(img.width) * img.height * 4);  // RGBA8_UNORM, row 0 = the top
    for (std::uint32_t y = 0; y < img.height; ++y)
        std::memcpy(rgba8->data() + std::size_t(y) * img.width * 4, static_cast<const std::uint8_t *>(m.pData) + std::size_t(y) * m.RowPitch,
                    std::size_t(img.width) * 4);
    m_cx->Unmap(staging.Get(), 0);
    return validationProblem();
}

std::string D3D11Backend::readback(double t, std::vector<std::uint8_t> *rgba8)
{
    if (!m_loaded)
        return "readback() before load()";
    // §5.7: the workload's output before the present pass, exactly render size.
    if (std::string e = waitAll(); !e.empty())
        return e;
    if (!tonemapped()) {
        if (std::string e = recordWorkload(t); !e.empty())
            return e;
        return readImage(outputImage(), rgba8);
    }
    // A tone mapped scene (sceneColor is RGBA16F, linear): the present pass in
    // mode 0, no overlay, into an RGBA8 target at render size (task 11's rule).
    Image &target = m_work->readTarget;
    const PresentConstants pc = presentConstants(kPresentTonemap, std::string(), 1.0, target.width, target.height);
    std::memcpy(m_frameShadow.data() + kPassBase, &pc, sizeof pc);
    if (std::string e = recordWorkload(t); !e.empty())
        return e;
    recordPresentPass(target.rtv.Get(), target.width, target.height);
    return readImage(target, rgba8);
}

#if defined(SYNBENCH_DEV_HOOKS)
std::string D3D11Backend::readbackPresented(std::vector<std::uint8_t> *rgba8)
{
    if (!m_headless || !m_resumed || m_frame == 0)
        return "readbackPresented() needs a headless frame";
    return readImage(m_headlessTarget, rgba8);
}
#endif

} // namespace sb::gpu::d3d11

namespace sb::gpu {
std::unique_ptr<IBackend> makeD3D11Backend()
{
    return std::make_unique<d3d11::D3D11Backend>();
}
} // namespace sb::gpu
