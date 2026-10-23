// SPDX-License-Identifier: GPL-3.0-or-later
// The D3D11 scenes (Phase 4a design §6.1-§6.4, task 20): the D3D12 path of
// task 19 (the Vulkan path of tasks 11 and 12, with the D2 fog and the open
// Shading ceiling) one to one. The same HLSL as DXBC (FXC, SM 5.0), the same
// constants (scene_constants.h) and the same passes and clears, so the images
// match Vulkan's within §5.7's tolerance (D20). D3D11 tracks no resource
// states, so the barrier table of D3D12 becomes one rule: DX11-1, every pass
// first unbinds the SRVs (PSSetShaderResources with nulls), then sets its
// render targets, then binds its own source textures, so a texture is never
// an SRV and a render target at once (the runtime would null the SRV silently).
// Per-object constants are the object buffer's range 256 i through
// VSSetConstantBuffers1 / PSSetConstantBuffers1; materials and pass sources
// are SRV tables (t0-t5). Windows only.

#include "tests/gpu/worker/d3d11/d11_workload_state.h"

#include "tests/gpu/worker/scene_constants.h"

#include <cstddef>
#include <cstring>

namespace {
// The Windows build's DXBC (worker/shaders/CMakeLists.txt, SYNBENCH_GPU_SHADER_DIR).
#include "depth_only_vs.dxbc.inc"
#include "drawcalls_ps.dxbc.inc"
#include "drawcalls_vs.dxbc.inc"
#include "geometry_ps.dxbc.inc"
#include "geometry_vs.dxbc.inc"
#include "ground_ps.dxbc.inc"
#include "ground_vs.dxbc.inc"
#include "particles_ps.dxbc.inc"
#include "particles_vs.dxbc.inc"
#include "post_bright_ps.dxbc.inc"
#include "post_composite_ps.dxbc.inc"
#include "post_down_ps.dxbc.inc"
#include "post_fxaa_ps.dxbc.inc"
#include "post_tonemap_ps.dxbc.inc"
#include "post_up_ps.dxbc.inc"
#include "present_vs.dxbc.inc"
#include "quads_ps.dxbc.inc"
#include "quads_vs.dxbc.inc"
#include "sprites_ps.dxbc.inc"
#include "sprites_vs.dxbc.inc"
#include "shading_ps.dxbc.inc"
#include "shading_vs.dxbc.inc"
#include "sky_ps.dxbc.inc"
#include "sky_vs.dxbc.inc"
} // namespace

namespace sb::gpu::d3d11 {

namespace {

// A pipeline with the vertex shader, and the pixel shader when there is one.
PipelineDesc shaders(const unsigned char *vs, std::size_t vsSize, const unsigned char *ps = nullptr, std::size_t psSize = 0)
{
    PipelineDesc p;
    p.vs = vs;
    p.vsSize = vsSize;
    p.ps = ps;
    p.psSize = psSize;
    return p;
}

} // namespace

bool D3D11Backend::tonemapped() const
{
    return m_work && m_work->tonemap;
}

Image &D3D11Backend::outputImage()
{
    if (m_work && m_work->workload == Workload::Particles)
        return m_ldrB;  // §6.4: the chain ends in ldrB (FXAA), presented in Copy mode
    return tonemapped() ? m_sceneColor : m_ldrA;
}

// The vertex and index buffers of every mesh (immutable, uploaded once).
std::string D3D11Backend::uploadMeshes(const SceneData &scene)
{
    WorkloadState &w = *m_work;
    for (const Mesh &mesh : scene.meshes) {
        ComPtr<ID3D11Buffer> vb, ib;
        if (std::string e = uploadBuffer(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex), D3D11_BIND_VERTEX_BUFFER, &vb); !e.empty())
            return e;
        if (std::string e = uploadBuffer(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t), D3D11_BIND_INDEX_BUFFER, &ib); !e.empty())
            return e;
        w.vertexBuffers.push_back(vb);
        w.indexBuffers.push_back(ib);
        w.indexCounts.push_back(std::uint32_t(mesh.indices.size()));
    }
    return {};
}

std::string D3D11Backend::loadScene(const SceneData &scene)
{
    ID3D11Device *d = m_device.Get();
    WorkloadState &w = *m_work;
    w.tonemap = true;
    w.scene.workload = scene.workload;
    w.scene.instances = scene.instances;
    w.scene.spins = scene.spins;
    w.draws = std::uint32_t(scene.instances.size());
    const bool drawCalls = scene.workload == Workload::DrawCalls;
    const std::uint32_t W = m_init.renderWidth, H = m_init.renderHeight;

    // The render targets (§5.3): sceneColor RGBA16F, sceneDepth D32F, the RGBA8 readback target.
    if (std::string e = makeTarget(m_sceneColor, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, Kind::Colour); !e.empty())
        return e;
    if (std::string e = makeTarget(m_sceneDepth, W, H, DXGI_FORMAT_D32_FLOAT, Kind::Depth); !e.empty())
        return e;
    if (std::string e = makeTarget(w.readTarget, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, Kind::Colour); !e.empty())
        return e;
    if (std::string e = uploadMeshes(scene); !e.empty())
        return e;
    // Draw calls: 16 materials, one table each (t0).
    for (const Texture &tex : scene.textures) {
        Image img;
        if (std::string e = uploadTexture(tex, &img); !e.empty())
            return e;
        w.materials.push_back(img);
    }
    for (const Image &img : w.materials)
        w.materialTables.push_back(makeTable({&img}));
    PipelineDesc p = drawCalls ? shaders(kDrawcallsVsDxbc, kDrawcallsVsDxbcSize, kDrawcallsPsDxbc, kDrawcallsPsDxbcSize)
                               : shaders(kGeometryVsDxbc, kGeometryVsDxbcSize, kGeometryPsDxbc, kGeometryPsDxbcSize);
    p.input = PipelineDesc::Input::Mesh;
    p.depthTest = p.depthWrite = true;
    p.compare = D3D11_COMPARISON_LESS;
    return makePipeline(d, p, &w.pipeline);
}

std::string D3D11Backend::loadShading(const SceneData &scene)
{
    ID3D11Device *d = m_device.Get();
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
    if (std::string e = makeTarget(m_sceneColor, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, Kind::Colour); !e.empty())
        return e;
    if (std::string e = makeTarget(m_sceneDepth, W, H, DXGI_FORMAT_D32_FLOAT, Kind::Depth); !e.empty())
        return e;
    // The shadow map: R32_TYPELESS, a D32_FLOAT DSV and an R32_FLOAT SRV (t4).
    if (std::string e = makeTarget(m_shadow, sc::kShadowSize, sc::kShadowSize, DXGI_FORMAT_R32_TYPELESS, Kind::SampledDepth); !e.empty())
        return e;
    if (std::string e = makeTarget(w.readTarget, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, Kind::Colour); !e.empty())
        return e;
    if (std::string e = uploadMeshes(scene); !e.empty())
        return e;
    for (const Texture &tex : scene.textures) {
        Image img;
        if (std::string e = uploadTexture(tex, &img); !e.empty())
            return e;
        w.materials.push_back(img);
    }
    if (w.materials.size() != 4)
        return "the Shading scene needs 4 textures";
    // t0-t3 the textures, t4 the shadow map (Vulkan: set 2 b0-b3 + set 0 b2).
    w.materialTables.push_back(makeTable({&w.materials[0], &w.materials[1], &w.materials[2], &w.materials[3], &m_shadow}));

    PipelineDesc shadow = shaders(kDepthOnlyVsDxbc, kDepthOnlyVsDxbcSize);
    shadow.input = PipelineDesc::Input::Mesh;
    shadow.depthTest = shadow.depthWrite = true;
    shadow.compare = D3D11_COMPARISON_LESS;
    shadow.depthBias = true;
    if (std::string e = makePipeline(d, shadow, &w.shadowPipeline); !e.empty())
        return e;
    PipelineDesc prepass = shadow;
    prepass.depthBias = false;
    if (std::string e = makePipeline(d, prepass, &w.prepassPipeline); !e.empty())
        return e;
    PipelineDesc shade = shaders(kShadingVsDxbc, kShadingVsDxbcSize, kShadingPsDxbc, kShadingPsDxbcSize);
    shade.input = PipelineDesc::Input::Mesh;
    shade.depthTest = true;
    shade.depthWrite = false;
    shade.compare = D3D11_COMPARISON_EQUAL;
    return makePipeline(d, shade, &w.pipeline);
}

std::string D3D11Backend::loadParticles(const SceneData &scene)
{
    ID3D11Device *d = m_device.Get();
    WorkloadState &w = *m_work;
    w.tonemap = false;  // the chain tone maps itself; the present pass copies ldrB
    w.scene.workload = scene.workload;
    w.scene.instances = scene.instances;
    w.scene.spins = scene.spins;
    const std::uint32_t W = m_init.renderWidth, H = m_init.renderHeight;
    const DXGI_FORMAT hdr = DXGI_FORMAT_R16G16B16A16_FLOAT, ldr = DXGI_FORMAT_R8G8B8A8_UNORM;
    // sceneColor, sceneDepth (not sampled: D2), bloom[0..5], hdrB, ldrB; ldrA is load()'s.
    if (std::string e = makeTarget(m_sceneColor, W, H, hdr, Kind::Colour); !e.empty())
        return e;
    if (std::string e = makeTarget(m_sceneDepth, W, H, DXGI_FORMAT_D32_FLOAT, Kind::Depth); !e.empty())
        return e;
    for (std::uint32_t k = 0; k < m_bloom.size(); ++k) {
        if (std::string e = makeTarget(m_bloom[k], sc::bloomSize(W, k), sc::bloomSize(H, k), hdr, Kind::Colour); !e.empty())
            return e;
    }
    if (std::string e = makeTarget(m_hdrB, W, H, hdr, Kind::Colour); !e.empty())
        return e;
    if (std::string e = makeTarget(m_ldrB, W, H, ldr, Kind::Colour); !e.empty())
        return e;
    if (std::string e = uploadMeshes(scene); !e.empty())
        return e;
    w.particleCount = std::uint32_t(scene.particles.size());
    if (scene.particles.empty())
        return "the Particles scene has no particles";
    if (std::string e = uploadBuffer(scene.particles.data(), scene.particles.size() * sizeof(Particle), D3D11_BIND_VERTEX_BUFFER, &w.particles); !e.empty())
        return e;
    if (scene.textures.size() != 1)
        return "the Particles scene needs its disc texture";
    Image disc;
    if (std::string e = uploadTexture(scene.textures[0], &disc); !e.empty())
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

    PipelineDesc sky = shaders(kSkyVsDxbc, kSkyVsDxbcSize, kSkyPsDxbc, kSkyPsDxbcSize);
    sky.depthTest = true;
    sky.compare = D3D11_COMPARISON_LESS_EQUAL;  // at the far plane, against the cleared 1.0
    if (std::string e = makePipeline(d, sky, &w.skyPipeline); !e.empty())
        return e;
    PipelineDesc ground = shaders(kGroundVsDxbc, kGroundVsDxbcSize, kGroundPsDxbc, kGroundPsDxbcSize);  // geometry.hlsl's lighting plus the height fog (D2)
    ground.input = PipelineDesc::Input::Mesh;
    ground.depthTest = ground.depthWrite = true;
    ground.compare = D3D11_COMPARISON_LESS;
    if (std::string e = makePipeline(d, ground, &w.pipeline); !e.empty())
        return e;
    PipelineDesc parts = shaders(kParticlesVsDxbc, kParticlesVsDxbcSize, kParticlesPsDxbc, kParticlesPsDxbcSize);
    parts.input = PipelineDesc::Input::Particle;
    parts.depthTest = true;
    parts.compare = D3D11_COMPARISON_LESS_EQUAL;  // tested, not written
    parts.additive = true;
    if (std::string e = makePipeline(d, parts, &w.particlePipeline); !e.empty())
        return e;
    const auto post = [&](const unsigned char *ps, std::size_t bytes, bool additive, Pipeline *out) {
        PipelineDesc p = shaders(kPresentVsDxbc, kPresentVsDxbcSize, ps, bytes);  // §13.1: the post passes reuse present's full-screen VS
        p.additive = additive;
        return makePipeline(d, p, out);
    };
    for (std::string e : {post(kPostBrightPsDxbc, kPostBrightPsDxbcSize, false, &w.brightPipeline),
                          post(kPostDownPsDxbc, kPostDownPsDxbcSize, false, &w.downPipeline),
                          post(kPostUpPsDxbc, kPostUpPsDxbcSize, true, &w.upPipeline),
                          post(kPostCompositePsDxbc, kPostCompositePsDxbcSize, false, &w.compositePipeline),
                          post(kPostTonemapPsDxbc, kPostTonemapPsDxbcSize, false, &w.tonemapPipeline),
                          post(kPostFxaaPsDxbc, kPostFxaaPsDxbcSize, false, &w.fxaaPipeline)}) {
        if (!e.empty())
            return e;
    }
    w.draws = 3 + sc::kPostPasses;
    return {};
}

std::string D3D11Backend::writeConstants(double t)
{
    WorkloadState &w = *m_work;
    // The CPU side: the same bytes as Vulkan and D3D12 (scene_constants.h), written to the CPU copy of the frame buffer.
    sc::writeFrame(w.scene, t, m_sceneColor.width, m_sceneColor.height, m_frameShadow.data());
    if (m_objectCb) {
        // DX11-2: Map(WRITE_DISCARD) on the whole object buffer once per frame; the draws then bind ranges of it.
        D3D11_MAPPED_SUBRESOURCE m{};
        if (const HRESULT hr = m_cx->Map(m_objectCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m); FAILED(hr))
            return dxgi::failure("Map (object constants)", hr);
        sc::writeObjects(w.scene, t, static_cast<std::uint8_t *>(m.pData));
        m_cx->Unmap(m_objectCb.Get(), 0);
    }
    return flushFrameConstants();
}

// The per-object draw loop of every scene (§6.2's rule: the mesh's buffers when
// they differ from the previous draw's, the material's table when it differs,
// the object's constants always, then the indexed draw).
std::uint32_t D3D11Backend::drawObjects(bool materials)
{
    WorkloadState &w = *m_work;
    std::uint32_t draws = 0, lastMesh = UINT32_MAX, lastMaterial = UINT32_MAX;
    for (std::size_t i = 0; i < w.scene.instances.size(); ++i) {
        const Instance &inst = w.scene.instances[i];
        if (inst.mesh != lastMesh) {
            ID3D11Buffer *vb = w.vertexBuffers[inst.mesh].Get();
            const UINT stride = sizeof(Vertex), offset = 0;
            m_cx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            m_cx->IASetIndexBuffer(w.indexBuffers[inst.mesh].Get(), DXGI_FORMAT_R32_UINT, 0);
            lastMesh = inst.mesh;
        }
        if (materials && inst.material != lastMaterial) {
            bindTable(w.materialTables[inst.material]);
            lastMaterial = inst.material;
        }
        setConstants(1, m_objectCb.Get(), UINT(i * sc::kObjectSlot), UINT(sc::kObjectSlot), m_psBound);
        m_cx->DrawIndexed(w.indexCounts[inst.mesh], 0, 0);
        ++draws;
    }
    return draws;
}

std::string D3D11Backend::recordScene(double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;
    if (std::string e = writeConstants(t); !e.empty())
        return e;
    // Vulkan: sceneColor UNDEFINED -> COLOR_ATTACHMENT, sceneDepth UNDEFINED -> DEPTH_ATTACHMENT; both cleared.
    unbindSrvs();  // DX11-1: last frame's present pass sampled sceneColor
    ID3D11RenderTargetView *rtv = m_sceneColor.rtv.Get();
    m_cx->OMSetRenderTargets(1, &rtv, m_sceneDepth.dsv.Get());
    m_cx->ClearRenderTargetView(rtv, sc::kBackground);
    m_cx->ClearDepthStencilView(m_sceneDepth.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    setViewport(width, height);
    bindPipeline(w.pipeline);
    setConstants(2, m_frameCb.Get(), sc::kPresentOffset, sc::kBlock);  // unused by the scene shaders; a valid range
    m_lastDraws = drawObjects(!w.materialTables.empty());
    return {};
}

std::string D3D11Backend::recordShading(double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;
    if (std::string e = writeConstants(t); !e.empty())
        return e;
    std::uint32_t draws = 0;

    // 1. The shadow pass: depth only into the shadow map (slope bias 2.0 in the rasteriser state,
    //    the constant part in depth_only.hlsl). Last frame's shading pass read it as t4.
    unbindSrvs();  // DX11-1
    m_cx->OMSetRenderTargets(0, nullptr, m_shadow.dsv.Get());
    m_cx->ClearDepthStencilView(m_shadow.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    setViewport(m_shadow.width, m_shadow.height);
    bindPipeline(w.shadowPipeline);
    setConstants(2, m_frameCb.Get(), UINT(sc::blockOffset(sc::kShadowBlock)), sc::kBlock, false);
    draws += drawObjects(false);

    // 2. The depth pre-pass into sceneDepth (LESS, written).
    m_cx->OMSetRenderTargets(0, nullptr, m_sceneDepth.dsv.Get());
    m_cx->ClearDepthStencilView(m_sceneDepth.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    setViewport(width, height);
    bindPipeline(w.prepassPipeline);
    setConstants(2, m_frameCb.Get(), UINT(sc::blockOffset(sc::kPrepassBlock)), sc::kBlock, false);
    draws += drawObjects(false);

    // 3. The shading pass: EQUAL, no depth writes, into sceneColor (cleared); the shadow map is
    //    only an SRV now (t4), bound after the render targets are set.
    ID3D11RenderTargetView *rtv = m_sceneColor.rtv.Get();
    m_cx->OMSetRenderTargets(1, &rtv, m_sceneDepth.dsv.Get());
    m_cx->ClearRenderTargetView(rtv, sc::kBackground);
    bindPipeline(w.pipeline);
    setConstants(2, m_frameCb.Get(), UINT(sc::kLightsOffset), UINT(sc::kLightsBytes));
    bindTable(w.materialTables[0]);
    draws += drawObjects(false);
    m_lastDraws = draws;
    return {};
}

void D3D11Backend::postPass(Image &target, const Pipeline &pipeline, const Table &table, std::uint32_t block)
{
    // An upsample keeps its target's contents and blends (the pipeline's ONE+ONE); every other pass overwrites it.
    unbindSrvs();  // DX11-1: the previous pass's source may be this pass's target
    ID3D11RenderTargetView *rtv = target.rtv.Get();
    m_cx->OMSetRenderTargets(1, &rtv, nullptr);
    setViewport(target.width, target.height);
    bindPipeline(pipeline);
    setConstants(2, m_frameCb.Get(), UINT(sc::blockOffset(block)), sc::kBlock);
    bindTable(table);
    m_cx->Draw(3, 0);
}

std::string D3D11Backend::recordParticles(double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;
    if (std::string e = writeConstants(t); !e.empty())
        return e;

    // The scene pass: sky, ground, particles into sceneColor + sceneDepth (both cleared).
    unbindSrvs();  // DX11-1
    ID3D11RenderTargetView *rtv = m_sceneColor.rtv.Get();
    m_cx->OMSetRenderTargets(1, &rtv, m_sceneDepth.dsv.Get());
    m_cx->ClearRenderTargetView(rtv, sc::kBackground);
    m_cx->ClearDepthStencilView(m_sceneDepth.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    setViewport(width, height);
    setConstants(2, m_frameCb.Get(), sc::kPresentOffset, sc::kBlock);  // no pass block used here; a valid range
    bindPipeline(w.skyPipeline);
    m_cx->Draw(3, 0);
    bindPipeline(w.pipeline);
    std::uint32_t draws = 1 + drawObjects(false);
    bindPipeline(w.particlePipeline);
    bindTable(w.materialTables[0]);
    ID3D11Buffer *particles = w.particles.Get();
    const UINT stride = sizeof(Particle), offset = 0;
    m_cx->IASetVertexBuffers(0, 1, &particles, &stride, &offset);
    m_cx->DrawInstanced(6, w.particleCount, 0, 0);
    ++draws;

    // The 14 post passes (§6.4), each on present's full-screen VS. Each pass's sources are
    // bound after its target is set (postPass), so scene colour's writes are read by the first.
    std::uint32_t n = 0;
    postPass(m_bloom[0], w.brightPipeline, w.postTables[n], n), ++n;  // 1
    for (std::size_t k = 1; k < m_bloom.size(); ++k)
        postPass(m_bloom[k], w.downPipeline, w.postTables[n], n), ++n;  // 2-6
    for (std::size_t k = m_bloom.size() - 1; k >= 1; --k)
        postPass(m_bloom[k - 1], w.upPipeline, w.postTables[n], n), ++n;  // 7-11
    postPass(m_hdrB, w.compositePipeline, w.postTables[n], n), ++n;  // 12
    postPass(m_ldrA, w.tonemapPipeline, w.postTables[n], n), ++n;    // 13
    // 14: FXAA into ldrB, left the bound render target: frame() / readback() read it (outputImage()).
    postPass(m_ldrB, w.fxaaPipeline, w.postTables[n], n), ++n;
    m_lastDraws = draws + n;
    return {};
}

// ---- Task 30: the fill workloads, Quads and Sprites (§6.5, §6.6) ----

std::string D3D11Backend::loadFill(const SceneData &scene)
{
    WorkloadState &w = *m_work;
    w.tonemap = false;  // display-encoded colour into ldrA, presented in Copy mode
    w.scene.workload = scene.workload;
    w.scene.sprites = scene.sprites;
    const bool quads = scene.workload == Workload::Quads;
    w.fillCount = quads ? scene.quadLayers : std::uint32_t(scene.sprites.size());
    if (scene.textures.size() != 1)
        return "a fill workload needs its one texture";
    Image tex;
    if (std::string e = uploadTexture(scene.textures[0], &tex); !e.empty())
        return e;
    w.materials.push_back(tex);
    w.materialTables.push_back(makeTable({&w.materials[0]}));
    PipelineDesc p = quads ? shaders(kQuadsVsDxbc, kQuadsVsDxbcSize, kQuadsPsDxbc, kQuadsPsDxbcSize)
                           : shaders(kSpritesVsDxbc, kSpritesVsDxbcSize, kSpritesPsDxbc, kSpritesPsDxbcSize);
    p.input = quads ? PipelineDesc::Input::None : PipelineDesc::Input::Sprite;
    p.alphaBlend = true;
    if (std::string e = makePipeline(m_device.Get(), p, &w.pipeline); !e.empty())
        return e;
    if (!quads && w.fillCount > 0) {
        // DX11-2: a DYNAMIC vertex buffer, Map(WRITE_DISCARD) once per frame, the batches' offsets through StartInstanceLocation.
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(w.fillCount * sc::kSpriteBytes);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (const HRESULT hr = m_device->CreateBuffer(&bd, nullptr, &m_spriteCb); FAILED(hr))
            return dxgi::failure("CreateBuffer (sprites)", hr);
    }
    if (!quads)
        w.spriteBatches = sc::spriteBatches(w.fillCount);
    w.draws = quads ? 1 : std::uint32_t(w.spriteBatches.size());
    return {};
}

std::string D3D11Backend::recordFill(double t)
{
    WorkloadState &w = *m_work;
    const bool quads = w.workload == Workload::Quads;
    const std::uint32_t width = m_ldrA.width, height = m_ldrA.height;
    sc::writeFrame(w.scene, t, width, height, m_frameShadow.data());
    if (std::string e = flushFrameConstants(); !e.empty())
        return e;
    if (!quads && m_spriteCb) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (const HRESULT hr = m_cx->Map(m_spriteCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m); FAILED(hr))
            return dxgi::failure("Map (sprites)", hr);
        sc::writeSprites(w.scene, t, static_cast<std::uint8_t *>(m.pData));
        m_cx->Unmap(m_spriteCb.Get(), 0);
    }
    unbindSrvs();  // DX11-1: last frame's present pass sampled ldrA
    ID3D11RenderTargetView *rtv = m_ldrA.rtv.Get();
    m_cx->OMSetRenderTargets(1, &rtv, nullptr);
    m_cx->ClearRenderTargetView(rtv, sc::kFillClear);
    std::uint32_t draws = 0;
    if (w.fillCount > 0) {
        setViewport(width, height);
        bindPipeline(w.pipeline);
        setConstants(2, m_frameCb.Get(), sc::kPresentOffset, sc::kBlock);  // unused by the fill shaders; a valid range
        bindTable(w.materialTables[0]);
        if (quads) {
            m_cx->DrawInstanced(6, w.fillCount, 0, 0);  // one instanced draw: a layer per instance
            draws = 1;
        } else {
            ID3D11Buffer *ring = m_spriteCb.Get();
            const UINT stride = UINT(sc::kSpriteBytes), offset = 0;
            m_cx->IASetVertexBuffers(0, 1, &ring, &stride, &offset);
            for (const sc::SpriteBatch &b : w.spriteBatches) {
                m_cx->DrawInstanced(6, b.count, 0, b.first);  // StartInstanceLocation = the batch's first sprite
                ++draws;
            }
        }
    }
    m_lastDraws = draws;
    return {};
}

} // namespace sb::gpu::d3d11
