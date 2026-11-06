// SPDX-License-Identifier: GPL-3.0-or-later
// The D3D12 scenes (Phase 4a design §6.1-§6.4, task 19): the Vulkan path of
// tasks 11 and 12 (with the D2 fog and the open Shading ceiling) one to one.
// The same HLSL, the same constants (scene_constants.h) and the same passes,
// so the images match Vulkan's within §5.7's tolerance (D20). The state
// transitions mirror task 12's barrier table; the D3D12 table is in the design
// Log. Per-object constants are a root CBV set to the slot's address + 256 i
// per draw; materials and pass sources are SRV tables (t0-t5). Windows only.

#include "tests/gpu/worker/d3d12/d12_workload_state.h"

#include "tests/gpu/worker/scene_constants.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace {
// The Windows build's DXIL (worker/shaders/CMakeLists.txt, SYNTHEON_GPU_SHADER_DIR).
#include "depth_only_vs.dxil.inc"
#include "drawcalls_ps.dxil.inc"
#include "drawcalls_vs.dxil.inc"
#include "geometry_ps.dxil.inc"
#include "geometry_vs.dxil.inc"
#include "ground_ps.dxil.inc"
#include "ground_vs.dxil.inc"
#include "particles_ps.dxil.inc"
#include "particles_vs.dxil.inc"
#include "post_bright_ps.dxil.inc"
#include "post_composite_ps.dxil.inc"
#include "post_down_ps.dxil.inc"
#include "post_fxaa_ps.dxil.inc"
#include "post_tonemap_ps.dxil.inc"
#include "post_up_ps.dxil.inc"
#include "present_vs.dxil.inc"
#include "quads_ps.dxil.inc"
#include "quads_vs.dxil.inc"
#include "sprites_ps.dxil.inc"
#include "sprites_vs.dxil.inc"
#include "shading_ps.dxil.inc"
#include "shading_vs.dxil.inc"
#include "sky_ps.dxil.inc"
#include "sky_vs.dxil.inc"
} // namespace

namespace sb::gpu::d3d12 {

namespace {

constexpr D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// A graphics pipeline on the one root signature (Vulkan's PipelineDesc).
struct PipelineDesc {
    D3D12_SHADER_BYTECODE vs{}, ps{};  // ps empty: no pixel shader (depth only)
    enum class Input { None, Mesh, Particle, Sprite } input = Input::None;
    DXGI_FORMAT colour = DXGI_FORMAT_UNKNOWN, depth = DXGI_FORMAT_UNKNOWN;
    bool depthTest = false, depthWrite = false;
    D3D12_COMPARISON_FUNC compare = D3D12_COMPARISON_FUNC_LESS;
    bool additive = false;   // blend ONE, ONE
    bool alphaBlend = false; // blend SRC_ALPHA, INV_SRC_ALPHA (colour), ONE, INV_SRC_ALPHA (alpha): the fill workloads
    bool depthBias = false;  // §6.3: DepthBias 0, SlopeScaledDepthBias 2.0 (Vulkan: vkCmdSetDepthBias(0, 0, 2))
};

std::string makePipeline(ID3D12Device *d, ID3D12RootSignature *root, const PipelineDesc &p, ComPtr<ID3D12PipelineState> *out)
{
    // The Vertex layout (32 bytes) by the HLSL semantics; Particle's two float4 per instance.
    const D3D12_INPUT_ELEMENT_DESC mesh[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, px), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, nx), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    const D3D12_INPUT_ELEMENT_DESC particle[] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    // §6.6: 16 bytes per sprite, one instance each: {float x, y; uint packed; uint rgba8}.
    const D3D12_INPUT_ELEMENT_DESC sprite[] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.VS = p.vs;
    pd.PS = p.ps;
    if (p.input == PipelineDesc::Input::Mesh)
        pd.InputLayout = {mesh, 3};
    else if (p.input == PipelineDesc::Input::Particle)
        pd.InputLayout = {particle, 2};
    else if (p.input == PipelineDesc::Input::Sprite)
        pd.InputLayout = {sprite, 3};
    D3D12_RENDER_TARGET_BLEND_DESC &rt = pd.BlendState.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (p.additive) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = rt.DestBlend = D3D12_BLEND_ONE;
        rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D12_BLEND_ONE;
        rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    } else if (p.alphaBlend) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // task 11: both faces on every API
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthBias = 0;
    pd.RasterizerState.DepthBiasClamp = 0.0f;
    pd.RasterizerState.SlopeScaledDepthBias = p.depthBias ? 2.0f : 0.0f;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = p.depthTest ? TRUE : FALSE;
    pd.DepthStencilState.DepthWriteMask = p.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = p.compare;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = p.colour != DXGI_FORMAT_UNKNOWN ? 1 : 0;
    pd.RTVFormats[0] = p.colour;
    pd.DSVFormat = p.depth;
    pd.SampleDesc.Count = 1;
    if (const HRESULT hr = d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(out->ReleaseAndGetAddressOf())); FAILED(hr))
        return dxgi::failure("CreateGraphicsPipelineState", hr);
    return {};
}

void setViewport(ID3D12GraphicsCommandList *list, std::uint32_t w, std::uint32_t h)
{
    // D3D's viewport is y-down, the same window mapping as Vulkan's negative-height one (VK-3).
    const D3D12_VIEWPORT vp{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &scissor);
}

// The vertex and index buffers of every mesh (DEFAULT heap, uploaded once).
template <class State>
std::string uploadMeshes(Context &ctx, const SceneData &scene, State &w)
{
    for (const Mesh &mesh : scene.meshes) {
        Buffer vb, ib;
        const UINT64 vbytes = mesh.vertices.size() * sizeof(Vertex), ibytes = mesh.indices.size() * sizeof(std::uint32_t);
        if (std::string e = makeBuffer(ctx, vbytes, D3D12_HEAP_TYPE_DEFAULT, &vb); !e.empty())
            return e;
        if (std::string e = uploadBuffer(ctx, vb, mesh.vertices.data(), std::size_t(vbytes)); !e.empty())
            return e;
        if (std::string e = makeBuffer(ctx, ibytes, D3D12_HEAP_TYPE_DEFAULT, &ib); !e.empty())
            return e;
        if (std::string e = uploadBuffer(ctx, ib, mesh.indices.data(), std::size_t(ibytes)); !e.empty())
            return e;
        w.vertexBuffers.push_back(vb);
        w.indexBuffers.push_back(ib);
        w.indexCounts.push_back(std::uint32_t(mesh.indices.size()));
    }
    return {};
}

D3D12_SHADER_BYTECODE code(const unsigned char *bytes, std::size_t size)
{
    return {bytes, size};
}

} // namespace

bool D3D12Backend::tonemapped() const
{
    return m_work && m_work->tonemap;
}

Image &D3D12Backend::outputImage()
{
    if (m_work && m_work->workload == Workload::Particles)
        return m_ldrB;  // §6.4: the chain ends in ldrB (FXAA), presented in Copy mode
    return tonemapped() ? m_sceneColor : m_ldrA;
}

std::string D3D12Backend::makeTarget(Image &img, std::uint32_t w, std::uint32_t h, DXGI_FORMAT format, bool depth, const float *clearColour)
{
    D3D12_CLEAR_VALUE clear{};
    if (depth) {
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
    } else {
        clear.Format = format;
        if (clearColour)
            std::memcpy(clear.Color, clearColour, sizeof clear.Color);
    }
    const D3D12_RESOURCE_FLAGS flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_RESOURCE_STATES initial = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (std::string e = makeImage(m_ctx, w, h, format, flags, 1, initial, &clear, &img); !e.empty())
        return e;
    if (depth) {
        if (m_dsvNext >= Context::kDsvCount)
            return "out of DSV descriptors";
        D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D32_FLOAT;  // also the R32_TYPELESS shadow map's
        dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        img.dsv = m_dsvNext++;
        m_ctx.device->CreateDepthStencilView(img.resource.Get(), &dd, m_ctx.dsv(img.dsv));
    } else {
        if (m_rtvNext >= Context::kRtvCount)
            return "out of RTV descriptors";
        img.rtv = m_rtvNext++;
        m_ctx.device->CreateRenderTargetView(img.resource.Get(), nullptr, m_ctx.rtv(img.rtv));
    }
    return {};
}

UINT D3D12Backend::makeTable(std::initializer_list<const Image *> images)
{
    const UINT base = m_srvNext;
    m_srvNext += 6;
    UINT i = 0;
    for (const Image *img : images)
        writeSrv(m_ctx, base + i++, img);
    for (; i < 6; ++i)
        writeSrv(m_ctx, base + i, nullptr);
    return base;
}

std::string D3D12Backend::loadScene(const SceneData &scene)
{
    ID3D12Device *d = m_ctx.device.Get();
    WorkloadState &w = *m_work;
    w.tonemap = true;
    w.scene.workload = scene.workload;
    w.scene.instances = scene.instances;
    w.scene.spins = scene.spins;
    w.draws = std::uint32_t(scene.instances.size());
    const bool drawCalls = scene.workload == Workload::DrawCalls;
    const std::uint32_t W = m_init.renderWidth, H = m_init.renderHeight;

    // The render targets (§5.3): sceneColor RGBA16F, sceneDepth D32F, the RGBA8 readback target.
    if (std::string e = makeTarget(m_sceneColor, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, false, sc::kBackground); !e.empty())
        return e;
    if (std::string e = makeTarget(m_sceneDepth, W, H, DXGI_FORMAT_D32_FLOAT, true, nullptr); !e.empty())
        return e;
    if (std::string e = makeTarget(w.readTarget, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false, nullptr); !e.empty())
        return e;
    if (std::string e = uploadMeshes(m_ctx, scene, w); !e.empty())
        return e;
    // Draw calls: 16 materials, one table each (t0).
    for (const Texture &tex : scene.textures) {
        Image img;
        if (std::string e = uploadTexture(m_ctx, tex, &img); !e.empty())
            return e;
        w.materials.push_back(img);
    }
    for (const Image &img : w.materials)
        w.materialTables.push_back(makeTable({&img}));
    PipelineDesc p;
    p.vs = drawCalls ? code(kDrawcallsVsDxil, kDrawcallsVsDxilSize) : code(kGeometryVsDxil, kGeometryVsDxilSize);
    p.ps = drawCalls ? code(kDrawcallsPsDxil, kDrawcallsPsDxilSize) : code(kGeometryPsDxil, kGeometryPsDxilSize);
    p.input = PipelineDesc::Input::Mesh;
    p.colour = DXGI_FORMAT_R16G16B16A16_FLOAT;
    p.depth = DXGI_FORMAT_D32_FLOAT;
    p.depthTest = p.depthWrite = true;
    p.compare = D3D12_COMPARISON_FUNC_LESS;
    return makePipeline(d, m_root.Get(), p, &w.pipeline);
}

std::string D3D12Backend::loadShading(const SceneData &scene)
{
    ID3D12Device *d = m_ctx.device.Get();
    WorkloadState &w = *m_work;
    w.tonemap = true;
    w.scene.workload = scene.workload;
    w.scene.instances = scene.instances;
    w.scene.spins = scene.spins;
    w.scene.lights = scene.lights;
    w.scene.lightMotion = scene.lightMotion;
    if (scene.lights.size() > kPointLights)
        return "more point lights than the Lights cbuffer holds";
    w.draws = 3 * std::uint32_t(scene.instances.size());
    const std::uint32_t W = m_init.renderWidth, H = m_init.renderHeight;
    if (std::string e = makeTarget(m_sceneColor, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, false, sc::kBackground); !e.empty())
        return e;
    if (std::string e = makeTarget(m_sceneDepth, W, H, DXGI_FORMAT_D32_FLOAT, true, nullptr); !e.empty())
        return e;
    // The shadow map: R32_TYPELESS, a D32_FLOAT DSV and an R32_FLOAT SRV (t4).
    if (std::string e = makeTarget(m_shadow, sc::kShadowSize, sc::kShadowSize, DXGI_FORMAT_R32_TYPELESS, true, nullptr); !e.empty())
        return e;
    if (std::string e = makeTarget(w.readTarget, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false, nullptr); !e.empty())
        return e;
    if (std::string e = uploadMeshes(m_ctx, scene, w); !e.empty())
        return e;
    for (const Texture &tex : scene.textures) {
        Image img;
        if (std::string e = uploadTexture(m_ctx, tex, &img); !e.empty())
            return e;
        w.materials.push_back(img);
    }
    if (w.materials.size() != 4)
        return "the Shading scene needs 4 textures";
    // t0-t3 the textures, t4 the shadow map (Vulkan: set 2 b0-b3 + set 0 b2).
    w.materialTables.push_back(makeTable({&w.materials[0], &w.materials[1], &w.materials[2], &w.materials[3], &m_shadow}));

    PipelineDesc shadow;
    shadow.vs = code(kDepthOnlyVsDxil, kDepthOnlyVsDxilSize);
    shadow.input = PipelineDesc::Input::Mesh;
    shadow.depth = DXGI_FORMAT_D32_FLOAT;
    shadow.depthTest = shadow.depthWrite = true;
    shadow.compare = D3D12_COMPARISON_FUNC_LESS;
    shadow.depthBias = true;
    if (std::string e = makePipeline(d, m_root.Get(), shadow, &w.shadowPipeline); !e.empty())
        return e;
    PipelineDesc prepass = shadow;
    prepass.depthBias = false;
    if (std::string e = makePipeline(d, m_root.Get(), prepass, &w.prepassPipeline); !e.empty())
        return e;
    PipelineDesc shade;
    shade.vs = code(kShadingVsDxil, kShadingVsDxilSize);
    shade.ps = code(kShadingPsDxil, kShadingPsDxilSize);
    shade.input = PipelineDesc::Input::Mesh;
    shade.colour = DXGI_FORMAT_R16G16B16A16_FLOAT;
    shade.depth = DXGI_FORMAT_D32_FLOAT;
    shade.depthTest = true;
    shade.depthWrite = false;
    shade.compare = D3D12_COMPARISON_FUNC_EQUAL;
    return makePipeline(d, m_root.Get(), shade, &w.pipeline);
}

std::string D3D12Backend::loadParticles(const SceneData &scene)
{
    ID3D12Device *d = m_ctx.device.Get();
    WorkloadState &w = *m_work;
    w.tonemap = false;  // the chain tone maps itself; the present pass copies ldrB
    w.scene.workload = scene.workload;
    w.scene.instances = scene.instances;
    w.scene.spins = scene.spins;
    const std::uint32_t W = m_init.renderWidth, H = m_init.renderHeight;
    const DXGI_FORMAT hdr = DXGI_FORMAT_R16G16B16A16_FLOAT, ldr = DXGI_FORMAT_R8G8B8A8_UNORM;
    // sceneColor, sceneDepth (not sampled: D2), bloom[0..5], hdrB, ldrB; ldrA is load()'s.
    if (std::string e = makeTarget(m_sceneColor, W, H, hdr, false, sc::kBackground); !e.empty())
        return e;
    if (std::string e = makeTarget(m_sceneDepth, W, H, DXGI_FORMAT_D32_FLOAT, true, nullptr); !e.empty())
        return e;
    for (std::uint32_t k = 0; k < m_bloom.size(); ++k) {
        if (std::string e = makeTarget(m_bloom[k], sc::bloomSize(W, k), sc::bloomSize(H, k), hdr, false, nullptr); !e.empty())
            return e;
    }
    if (std::string e = makeTarget(m_hdrB, W, H, hdr, false, nullptr); !e.empty())
        return e;
    if (std::string e = makeTarget(m_ldrB, W, H, ldr, false, nullptr); !e.empty())
        return e;
    if (std::string e = uploadMeshes(m_ctx, scene, w); !e.empty())
        return e;
    w.particleCount = std::uint32_t(scene.particles.size());
    if (std::string e = makeBuffer(m_ctx, std::max<UINT64>(scene.particles.size(), 1) * sizeof(Particle), D3D12_HEAP_TYPE_DEFAULT, &w.particles);
        !e.empty())
        return e;
    if (std::string e = uploadBuffer(m_ctx, w.particles, scene.particles.data(), scene.particles.size() * sizeof(Particle)); !e.empty())
        return e;
    if (scene.textures.size() != 1)
        return "the Particles scene needs its disc texture";
    Image disc;
    if (std::string e = uploadTexture(m_ctx, scene.textures[0], &disc); !e.empty())
        return e;
    w.materials.push_back(disc);
    w.materialTables.push_back(makeTable({&w.materials[0]}));

    // The post passes' tables, in pass order (Vulkan's post sets).
    std::size_t n = 0;
    w.postTables[n++] = makeTable({&m_sceneColor});  // 1 bright
    for (std::size_t k = 1; k < m_bloom.size(); ++k)
        w.postTables[n++] = makeTable({&m_bloom[k - 1]});  // 2-6 down
    for (std::size_t k = m_bloom.size() - 1; k >= 1; --k)
        w.postTables[n++] = makeTable({&m_bloom[k]});  // 7-11 up: [k] -> [k-1]
    w.postTables[n++] = makeTable({&m_sceneColor, &m_bloom[0]});  // 12 composite (no depth: D2)
    w.postTables[n++] = makeTable({&m_hdrB});                     // 13 tone map
    w.postTables[n++] = makeTable({&m_ldrA});                     // 14 FXAA

    PipelineDesc sky;
    sky.vs = code(kSkyVsDxil, kSkyVsDxilSize);
    sky.ps = code(kSkyPsDxil, kSkyPsDxilSize);
    sky.colour = hdr;
    sky.depth = DXGI_FORMAT_D32_FLOAT;
    sky.depthTest = true;
    sky.compare = D3D12_COMPARISON_FUNC_LESS_EQUAL;  // at the far plane, against the cleared 1.0
    if (std::string e = makePipeline(d, m_root.Get(), sky, &w.skyPipeline); !e.empty())
        return e;
    PipelineDesc ground = sky;
    ground.vs = code(kGroundVsDxil, kGroundVsDxilSize);  // geometry.hlsl's lighting plus the height fog (D2)
    ground.ps = code(kGroundPsDxil, kGroundPsDxilSize);
    ground.input = PipelineDesc::Input::Mesh;
    ground.depthWrite = true;
    ground.compare = D3D12_COMPARISON_FUNC_LESS;
    if (std::string e = makePipeline(d, m_root.Get(), ground, &w.pipeline); !e.empty())
        return e;
    PipelineDesc parts = sky;
    parts.vs = code(kParticlesVsDxil, kParticlesVsDxilSize);
    parts.ps = code(kParticlesPsDxil, kParticlesPsDxilSize);
    parts.input = PipelineDesc::Input::Particle;
    parts.compare = D3D12_COMPARISON_FUNC_LESS_EQUAL;  // tested, not written
    parts.additive = true;
    if (std::string e = makePipeline(d, m_root.Get(), parts, &w.particlePipeline); !e.empty())
        return e;
    const auto post = [&](const unsigned char *ps, std::size_t bytes, DXGI_FORMAT format, bool additive, ComPtr<ID3D12PipelineState> *out) {
        PipelineDesc p;
        p.vs = code(kPresentVsDxil, kPresentVsDxilSize);  // §13.1: the post passes reuse present's full-screen VS
        p.ps = code(ps, bytes);
        p.colour = format;
        p.additive = additive;
        return makePipeline(d, m_root.Get(), p, out);
    };
    for (std::string e : {post(kPostBrightPsDxil, kPostBrightPsDxilSize, hdr, false, &w.brightPipeline),
                          post(kPostDownPsDxil, kPostDownPsDxilSize, hdr, false, &w.downPipeline),
                          post(kPostUpPsDxil, kPostUpPsDxilSize, hdr, true, &w.upPipeline),
                          post(kPostCompositePsDxil, kPostCompositePsDxilSize, hdr, false, &w.compositePipeline),
                          post(kPostTonemapPsDxil, kPostTonemapPsDxilSize, ldr, false, &w.tonemapPipeline),
                          post(kPostFxaaPsDxil, kPostFxaaPsDxilSize, ldr, false, &w.fxaaPipeline)}) {
        if (!e.empty())
            return e;
    }
    w.draws = 3 + sc::kPostPasses;
    return {};
}

void D3D12Backend::unloadScene()
{
    if (!m_work)
        return;
    // After unload()'s wait for every slot (DX-2): nothing in flight uses these.
    m_work.reset();
}

// The per-object draw loop of every scene (§6.2's rule: the mesh's buffers when
// they differ from the previous draw's, the material's table when it differs,
// the object's constants always, then the indexed draw).
static std::uint32_t drawObjects(ID3D12GraphicsCommandList *list, const SceneData &scene, const std::vector<Buffer> &vertexBuffers,
                                 const std::vector<Buffer> &indexBuffers, const std::vector<std::uint32_t> &indexCounts,
                                 const std::vector<UINT> *materialTables, Context &ctx, D3D12_GPU_VIRTUAL_ADDRESS objects)
{
    std::uint32_t draws = 0, lastMesh = UINT32_MAX, lastMaterial = UINT32_MAX;
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        const Instance &inst = scene.instances[i];
        if (inst.mesh != lastMesh) {
            const D3D12_VERTEX_BUFFER_VIEW vbv{vertexBuffers[inst.mesh].resource->GetGPUVirtualAddress(),
                                               UINT(vertexBuffers[inst.mesh].size), sizeof(Vertex)};
            const D3D12_INDEX_BUFFER_VIEW ibv{indexBuffers[inst.mesh].resource->GetGPUVirtualAddress(), UINT(indexBuffers[inst.mesh].size),
                                              DXGI_FORMAT_R32_UINT};
            list->IASetVertexBuffers(0, 1, &vbv);
            list->IASetIndexBuffer(&ibv);
            lastMesh = inst.mesh;
        }
        if (materialTables && inst.material != lastMaterial) {
            list->SetGraphicsRootDescriptorTable(3, ctx.srvGpu((*materialTables)[inst.material]));
            lastMaterial = inst.material;
        }
        list->SetGraphicsRootConstantBufferView(1, objects + i * sc::kObjectSlot);
        list->DrawIndexedInstanced(indexCounts[inst.mesh], 1, 0, 0, 0);
        ++draws;
    }
    return draws;
}

void D3D12Backend::recordScene(ID3D12GraphicsCommandList *list, Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;
    // The CPU side, after the slot's fence (DX-2): the same bytes as Vulkan (scene_constants.h).
    sc::writeFrame(w.scene, t, width, height, static_cast<std::uint8_t *>(slot.mapped[0]));
    sc::writeObjects(w.scene, t, static_cast<std::uint8_t *>(slot.mapped[1]));

    // Vulkan: sceneColor UNDEFINED -> COLOR_ATTACHMENT, sceneDepth UNDEFINED -> DEPTH_ATTACHMENT; both cleared.
    transition(list, m_sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(list, m_sceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_ctx.rtv(m_sceneColor.rtv), dsv = m_ctx.dsv(m_sceneDepth.dsv);
    list->ClearRenderTargetView(rtv, sc::kBackground, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    setViewport(list, width, height);
    list->SetGraphicsRootSignature(m_root.Get());
    list->SetPipelineState(w.pipeline.Get());
    const D3D12_GPU_VIRTUAL_ADDRESS constants = slot.frameConstants->GetGPUVirtualAddress();
    list->SetGraphicsRootConstantBufferView(0, constants);
    list->SetGraphicsRootConstantBufferView(2, constants + sc::kPresentOffset);  // unused by the scene shaders; a valid address
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_lastDraws = drawObjects(list, w.scene, w.vertexBuffers, w.indexBuffers, w.indexCounts, w.materialTables.empty() ? nullptr : &w.materialTables,
                              m_ctx, slot.objectConstants->GetGPUVirtualAddress());
}

void D3D12Backend::recordShading(ID3D12GraphicsCommandList *list, Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;
    sc::writeFrame(w.scene, t, width, height, static_cast<std::uint8_t *>(slot.mapped[0]));
    sc::writeObjects(w.scene, t, static_cast<std::uint8_t *>(slot.mapped[1]));
    const D3D12_GPU_VIRTUAL_ADDRESS constants = slot.frameConstants->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS objects = slot.objectConstants->GetGPUVirtualAddress();
    list->SetGraphicsRootSignature(m_root.Get());
    list->SetGraphicsRootConstantBufferView(0, constants);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    std::uint32_t draws = 0;

    // 1. The shadow pass: depth only into the shadow map (slope bias 2.0 in the PSO,
    //    the constant part in depth_only.hlsl). Last frame's shading pass read it.
    transition(list, m_shadow, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = m_ctx.dsv(m_shadow.dsv);
    list->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    list->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
    setViewport(list, m_shadow.width, m_shadow.height);
    list->SetPipelineState(w.shadowPipeline.Get());
    list->SetGraphicsRootConstantBufferView(2, constants + sc::blockOffset(sc::kShadowBlock));
    draws += drawObjects(list, w.scene, w.vertexBuffers, w.indexBuffers, w.indexCounts, nullptr, m_ctx, objects);
    // Shadow depth writes -> the shading pass's pixel-shader reads.
    transition(list, m_shadow, kShaderRead);

    // 2. The depth pre-pass into sceneDepth (LESS, written).
    transition(list, m_sceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_ctx.dsv(m_sceneDepth.dsv);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    setViewport(list, width, height);
    list->SetPipelineState(w.prepassPipeline.Get());
    list->SetGraphicsRootConstantBufferView(2, constants + sc::blockOffset(sc::kPrepassBlock));
    draws += drawObjects(list, w.scene, w.vertexBuffers, w.indexBuffers, w.indexCounts, nullptr, m_ctx, objects);
    // Vulkan needs a depth barrier here (two rendering instances); D3D12 doesn't:
    // sceneDepth stays DEPTH_WRITE and depth accesses keep their order on the queue.

    // 3. The shading pass: EQUAL, no depth writes, into sceneColor (cleared).
    transition(list, m_sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_ctx.rtv(m_sceneColor.rtv);
    list->ClearRenderTargetView(rtv, sc::kBackground, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    list->SetPipelineState(w.pipeline.Get());
    list->SetGraphicsRootConstantBufferView(2, constants + sc::kLightsOffset);
    list->SetGraphicsRootDescriptorTable(3, m_ctx.srvGpu(w.materialTables[0]));
    draws += drawObjects(list, w.scene, w.vertexBuffers, w.indexBuffers, w.indexCounts, nullptr, m_ctx, objects);
    m_lastDraws = draws;
}

void D3D12Backend::postPass(ID3D12GraphicsCommandList *list, Slot &slot, Image &target, bool accumulate, bool toShaderRead,
                            ID3D12PipelineState *pipeline, UINT table, std::uint32_t block)
{
    // An upsample keeps its target's contents and blends (Vulkan: SHADER_READ ->
    // COLOR_ATTACHMENT, LOAD_OP_LOAD); every other pass overwrites it.
    transition(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET);
    (void)accumulate;  // D3D12 keeps the contents through the transition either way
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_ctx.rtv(target.rtv);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    setViewport(list, target.width, target.height);
    list->SetPipelineState(pipeline);
    list->SetGraphicsRootConstantBufferView(2, slot.frameConstants->GetGPUVirtualAddress() + sc::blockOffset(block));
    list->SetGraphicsRootDescriptorTable(3, m_ctx.srvGpu(table));
    list->DrawInstanced(3, 1, 0, 0);
    if (toShaderRead)
        transition(list, target, kShaderRead);  // this pass's writes -> the next passes' reads
}

void D3D12Backend::recordParticles(ID3D12GraphicsCommandList *list, Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;
    sc::writeFrame(w.scene, t, width, height, static_cast<std::uint8_t *>(slot.mapped[0]));
    sc::writeObjects(w.scene, t, static_cast<std::uint8_t *>(slot.mapped[1]));
    const D3D12_GPU_VIRTUAL_ADDRESS constants = slot.frameConstants->GetGPUVirtualAddress();

    // The scene pass: sky, ground, particles into sceneColor + sceneDepth (both cleared).
    transition(list, m_sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(list, m_sceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_ctx.rtv(m_sceneColor.rtv), dsv = m_ctx.dsv(m_sceneDepth.dsv);
    list->ClearRenderTargetView(rtv, sc::kBackground, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    setViewport(list, width, height);
    list->SetGraphicsRootSignature(m_root.Get());
    list->SetGraphicsRootConstantBufferView(0, constants);
    list->SetGraphicsRootConstantBufferView(2, constants + sc::kPresentOffset);  // no pass block used here; a valid address
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetPipelineState(w.skyPipeline.Get());
    list->DrawInstanced(3, 1, 0, 0);
    list->SetPipelineState(w.pipeline.Get());
    std::uint32_t draws = 1 + drawObjects(list, w.scene, w.vertexBuffers, w.indexBuffers, w.indexCounts, nullptr, m_ctx,
                                          slot.objectConstants->GetGPUVirtualAddress());
    list->SetPipelineState(w.particlePipeline.Get());
    list->SetGraphicsRootDescriptorTable(3, m_ctx.srvGpu(w.materialTables[0]));
    const D3D12_VERTEX_BUFFER_VIEW particles{w.particles.resource->GetGPUVirtualAddress(), UINT(w.particles.size), sizeof(Particle)};
    list->IASetVertexBuffers(0, 1, &particles);
    list->DrawInstanced(6, w.particleCount, 0, 0);
    ++draws;
    // Scene colour writes -> the post passes' reads (the depth isn't read: D2).
    transition(list, m_sceneColor, kShaderRead);

    // The 14 post passes (§6.4), each on present's full-screen VS.
    std::uint32_t n = 0;
    postPass(list, slot, m_bloom[0], false, true, w.brightPipeline.Get(), w.postTables[n], n), ++n;  // 1
    for (std::size_t k = 1; k < m_bloom.size(); ++k)
        postPass(list, slot, m_bloom[k], false, true, w.downPipeline.Get(), w.postTables[n], n), ++n;  // 2-6
    for (std::size_t k = m_bloom.size() - 1; k >= 1; --k)
        postPass(list, slot, m_bloom[k - 1], true, true, w.upPipeline.Get(), w.postTables[n], n), ++n;  // 7-11
    postPass(list, slot, m_hdrB, false, true, w.compositePipeline.Get(), w.postTables[n], n), ++n;  // 12
    postPass(list, slot, m_ldrA, false, true, w.tonemapPipeline.Get(), w.postTables[n], n), ++n;    // 13
    // 14: FXAA into ldrB, left a render target: frame() / readback() transition it (outputImage()).
    postPass(list, slot, m_ldrB, false, false, w.fxaaPipeline.Get(), w.postTables[n], n), ++n;
    m_lastDraws = draws + n;
}

// ---- Task 30: the fill workloads, Quads and Sprites (§6.5, §6.6) ----

std::string D3D12Backend::loadFill(const SceneData &scene)
{
    ID3D12Device *d = m_ctx.device.Get();
    WorkloadState &w = *m_work;
    w.tonemap = false;  // display-encoded colour into ldrA, presented in Copy mode
    w.scene.workload = scene.workload;
    w.scene.sprites = scene.sprites;
    const bool quads = scene.workload == Workload::Quads;
    w.fillCount = quads ? scene.quadLayers : std::uint32_t(scene.sprites.size());
    if (scene.textures.size() != 1)
        return "a fill workload needs its one texture";
    Image tex;
    if (std::string e = uploadTexture(m_ctx, scene.textures[0], &tex); !e.empty())
        return e;
    w.materials.push_back(tex);
    w.materialTables.push_back(makeTable({&w.materials[0]}));
    PipelineDesc p;
    p.vs = quads ? code(kQuadsVsDxil, kQuadsVsDxilSize) : code(kSpritesVsDxil, kSpritesVsDxilSize);
    p.ps = quads ? code(kQuadsPsDxil, kQuadsPsDxilSize) : code(kSpritesPsDxil, kSpritesPsDxilSize);
    p.input = quads ? PipelineDesc::Input::None : PipelineDesc::Input::Sprite;
    p.colour = DXGI_FORMAT_R8G8B8A8_UNORM;
    p.alphaBlend = true;
    if (std::string e = makePipeline(d, m_root.Get(), p, &w.pipeline); !e.empty())
        return e;
    if (!quads && w.fillCount > 0) {
        // §6.6, DX-2: a persistently mapped UPLOAD buffer per slot, rewritten every frame after the slot's fence.
        for (Slot &s : m_slots) {
            Buffer ring;
            if (std::string e = makeBuffer(m_ctx, UINT64(w.fillCount) * sc::kSpriteBytes, D3D12_HEAP_TYPE_UPLOAD, &ring); !e.empty())
                return e;
            s.spriteRing = ring.resource;
            s.mapped[2] = ring.mapped;
        }
    }
    if (!quads)
        w.spriteBatches = sc::spriteBatches(w.fillCount);
    w.draws = quads ? 1 : std::uint32_t(w.spriteBatches.size());
    return {};
}

void D3D12Backend::recordFill(ID3D12GraphicsCommandList *list, Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const bool quads = w.workload == Workload::Quads;
    const std::uint32_t width = m_ldrA.width, height = m_ldrA.height;
    sc::writeFrame(w.scene, t, width, height, static_cast<std::uint8_t *>(slot.mapped[0]));
    if (!quads && slot.mapped[2])
        sc::writeSprites(w.scene, t, static_cast<std::uint8_t *>(slot.mapped[2]));
    // DX-1: last frame's present pass sampled ldrA, a readback copied it.
    transition(list, m_ldrA, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_ctx.rtv(m_ldrA.rtv);
    list->ClearRenderTargetView(rtv, sc::kFillClear, 0, nullptr);
    std::uint32_t draws = 0;
    if (w.fillCount > 0) {
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        setViewport(list, width, height);
        list->SetGraphicsRootSignature(m_root.Get());
        list->SetPipelineState(w.pipeline.Get());
        const D3D12_GPU_VIRTUAL_ADDRESS constants = slot.frameConstants->GetGPUVirtualAddress();
        list->SetGraphicsRootConstantBufferView(0, constants);
        list->SetGraphicsRootConstantBufferView(1, constants);  // unused by the fill shaders; a valid address
        list->SetGraphicsRootConstantBufferView(2, constants + sc::kPresentOffset);
        list->SetGraphicsRootDescriptorTable(3, m_ctx.srvGpu(w.materialTables[0]));
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (quads) {
            list->DrawInstanced(6, w.fillCount, 0, 0);  // one instanced draw: a layer per instance
            draws = 1;
        } else {
            const D3D12_VERTEX_BUFFER_VIEW ring{slot.spriteRing->GetGPUVirtualAddress(), UINT(w.fillCount * sc::kSpriteBytes), UINT(sc::kSpriteBytes)};
            list->IASetVertexBuffers(0, 1, &ring);
            for (const sc::SpriteBatch &b : w.spriteBatches) {
                list->DrawInstanced(6, b.count, 0, b.first);  // StartInstanceLocation = the batch's first sprite
                ++draws;
            }
        }
    }
    m_lastDraws = draws;
}

} // namespace sb::gpu::d3d12
