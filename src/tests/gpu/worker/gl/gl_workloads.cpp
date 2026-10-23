// SPDX-License-Identifier: GPL-3.0-or-later
// The OpenGL scenes (Phase 4a design §6.1-§6.4, task 22): the D3D11 path of
// task 20 (the Vulkan path of tasks 11 and 12, with the D2 fog and the open
// Shading ceiling) on GL 3.3 core. The same HLSL as SPIRV-Cross GLSL, the same
// constants (scene_constants.h, written with flipV = 1) and the same passes
// and clears, so the images match Vulkan's within §5.7's tolerance (D20).
// Per-object constants are the object buffer's range 256 i through
// glBindBufferRange(GL_UNIFORM_BUFFER, 1, ...); pass constants are ranges of
// the slot's frame buffer at points 0 and 2; materials and pass sources are
// textures on units 0-5 (the program's sampler objects, useProgram()).
// Orientation (GL-1): every FBO image is bottom-up in memory; the post chain's
// sourceUv / inputPixel and the present pass flip through flipV, the shadow
// lookup and the tangent frame of shading.hlsl through cameraUp.w, and
// readback flips the rows. The shadow bias is glPolygonOffset(2.0, 0) plus the
// shader's constant part. Culling is off.

#include "tests/gpu/worker/gl/gl_workload_state.h"

#include <glad/gl.h>

#include "tests/gpu/worker/scene/textures.h"
#include "tests/gpu/worker/scene_constants.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
#include "tests/gpu/worker/shaders/generated/depth_only_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/depth_only_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/drawcalls_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/drawcalls_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/geometry_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/geometry_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/ground_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/ground_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/particles_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/particles_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_bright_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_bright_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_composite_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_composite_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_down_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_down_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_fxaa_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_fxaa_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_tonemap_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_tonemap_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_up_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/post_up_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/quads_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/quads_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/shading_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/shading_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/sky_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/sky_vs.glsl.inc"
#include "tests/gpu/worker/shaders/generated/sprites_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/sprites_vs.glsl.inc"
} // namespace

namespace sb::gpu::gl {

bool GlBackend::tonemapped() const
{
    return m_work && m_work->tonemap;
}

std::uint32_t GlBackend::outputTexture() const
{
    if (m_work && m_work->workload == Workload::Particles)
        return m_ldrB.texture;  // §6.4: the chain ends in ldrB (FXAA), presented in Copy mode
    return tonemapped() ? m_sceneColor.texture : m_ldrA.texture;
}

GlBackend::Target &GlBackend::outputTarget()
{
    if (m_work && m_work->workload == Workload::Particles)
        return m_ldrB;
    return tonemapped() ? m_sceneColor : m_ldrA;
}

std::string GlBackend::uploadTexture(const Texture &level0, std::uint32_t *id)
{
    const std::vector<Texture> chain = level0.mips ? mipChain(level0) : std::vector<Texture>{level0};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Not flipped (GL-1): mesh UVs and texelFetch rows address the CPU rows on every API.
    for (std::size_t level = 0; level < chain.size(); ++level)
        glTexImage2D(GL_TEXTURE_2D, GLint(level), GL_RGBA8, GLsizei(chain[level].width), GLsizei(chain[level].height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     chain[level].rgba8.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(chain.size()) - 1);
    glBindTexture(GL_TEXTURE_2D, 0);
    *id = tex;
    return {};
}

// The vertex and index buffers of every mesh, and a VAO per mesh holding both (locations 0-2: position, normal, uv).
std::string GlBackend::uploadMeshes(const SceneData &scene)
{
    WorkloadState &w = *m_work;
    for (const Mesh &mesh : scene.meshes) {
        GLuint vbo = 0, ibo = 0, vao = 0;
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ibo);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(mesh.vertices.size() * sizeof(Vertex)), mesh.vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(mesh.indices.size() * sizeof(std::uint32_t)), mesh.indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, px)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, nx)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, u)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        w.vertexBuffers.push_back(vbo);
        w.indexBuffers.push_back(ibo);
        w.meshVaos.push_back(vao);
        w.indexCounts.push_back(std::uint32_t(mesh.indices.size()));
    }
    return {};
}

std::string GlBackend::loadScene(const SceneData &scene)
{
    WorkloadState &w = *m_work;
    w.tonemap = true;
    w.scene.workload = scene.workload;
    w.scene.instances = scene.instances;
    w.scene.spins = scene.spins;
    w.draws = std::uint32_t(scene.instances.size());
    const bool drawCalls = scene.workload == Workload::DrawCalls;
    const std::uint32_t W = m_init.renderWidth, H = m_init.renderHeight;

    // The render targets (§5.3): sceneColor RGBA16F, sceneDepth D32F, the RGBA8 readback target.
    if (std::string e = makeTarget(m_sceneColor, W, H, GL_RGBA16F); !e.empty())
        return e;
    if (std::string e = makeDepthTarget(m_sceneDepth, W, H); !e.empty())
        return e;
    if (std::string e = makeTarget(w.readTarget, W, H); !e.empty())
        return e;
    if (std::string e = makeFbo(m_sceneColor.texture, m_sceneDepth.texture, &w.sceneFbo); !e.empty())
        return e;
    if (std::string e = uploadMeshes(scene); !e.empty())
        return e;
    // Draw calls: 16 materials, one texture each (unit 0).
    for (const Texture &tex : scene.textures) {
        std::uint32_t id = 0;
        if (std::string e = uploadTexture(tex, &id); !e.empty())
            return e;
        w.materials.push_back(id);
    }
    return drawCalls ? makeProgram("drawcalls", kDrawcallsVsGlsl, kDrawcallsPsGlsl, &w.scene3d)
                     : makeProgram("geometry", kGeometryVsGlsl, kGeometryPsGlsl, &w.scene3d);
}

std::string GlBackend::loadShading(const SceneData &scene)
{
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
    if (std::string e = makeTarget(m_sceneColor, W, H, GL_RGBA16F); !e.empty())
        return e;
    if (std::string e = makeDepthTarget(m_sceneDepth, W, H); !e.empty())
        return e;
    if (std::string e = makeDepthTarget(m_shadow, sc::kShadowSize, sc::kShadowSize); !e.empty())  // sampled through s2 (compare)
        return e;
    if (std::string e = makeTarget(w.readTarget, W, H); !e.empty())
        return e;
    if (std::string e = makeFbo(m_sceneColor.texture, m_sceneDepth.texture, &w.sceneFbo); !e.empty())
        return e;
    if (std::string e = makeFbo(0, m_sceneDepth.texture, &w.prepassFbo); !e.empty())
        return e;
    if (std::string e = makeFbo(0, m_shadow.texture, &w.shadowFbo); !e.empty())
        return e;
    if (std::string e = uploadMeshes(scene); !e.empty())
        return e;
    for (const Texture &tex : scene.textures) {
        std::uint32_t id = 0;
        if (std::string e = uploadTexture(tex, &id); !e.empty())
            return e;
        w.materials.push_back(id);
    }
    if (w.materials.size() != 4)
        return "the Shading scene needs 4 textures";
    if (std::string e = makeProgram("depth_only", kDepthOnlyVsGlsl, kDepthOnlyPsGlsl, &w.depthOnly); !e.empty())
        return e;
    return makeProgram("shading", kShadingVsGlsl, kShadingPsGlsl, &w.scene3d);
}

std::string GlBackend::loadParticles(const SceneData &scene)
{
    WorkloadState &w = *m_work;
    w.tonemap = false;  // the chain tone maps itself; the present pass copies ldrB
    w.scene.workload = scene.workload;
    w.scene.instances = scene.instances;
    w.scene.spins = scene.spins;
    const std::uint32_t W = m_init.renderWidth, H = m_init.renderHeight;
    // sceneColor, sceneDepth (not sampled: D2), bloom[0..5], hdrB, ldrB; ldrA is load()'s.
    if (std::string e = makeTarget(m_sceneColor, W, H, GL_RGBA16F); !e.empty())
        return e;
    if (std::string e = makeDepthTarget(m_sceneDepth, W, H); !e.empty())
        return e;
    for (std::uint32_t k = 0; k < m_bloom.size(); ++k) {
        if (std::string e = makeTarget(m_bloom[k], sc::bloomSize(W, k), sc::bloomSize(H, k), GL_RGBA16F); !e.empty())
            return e;
    }
    if (std::string e = makeTarget(m_hdrB, W, H, GL_RGBA16F); !e.empty())
        return e;
    if (std::string e = makeTarget(m_ldrB, W, H); !e.empty())
        return e;
    if (std::string e = makeFbo(m_sceneColor.texture, m_sceneDepth.texture, &w.sceneFbo); !e.empty())
        return e;
    if (std::string e = uploadMeshes(scene); !e.empty())
        return e;
    if (scene.particles.empty())
        return "the Particles scene has no particles";
    w.particleCount = std::uint32_t(scene.particles.size());
    glGenBuffers(1, &w.particleBuffer);
    glGenVertexArrays(1, &w.particleVao);
    glBindVertexArray(w.particleVao);
    glBindBuffer(GL_ARRAY_BUFFER, w.particleBuffer);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(scene.particles.size() * sizeof(Particle)), scene.particles.data(), GL_STATIC_DRAW);
    // Two float4 per instance (locations 0 and 1, divisor 1); the six vertices come from gl_VertexID.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Particle), nullptr);
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Particle), reinterpret_cast<const void *>(16));
    glVertexAttribDivisor(1, 1);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (scene.textures.size() != 1)
        return "the Particles scene needs its disc texture";
    std::uint32_t disc = 0;
    if (std::string e = uploadTexture(scene.textures[0], &disc); !e.empty())
        return e;
    w.materials.push_back(disc);

    for (std::string e : {makeProgram("sky", kSkyVsGlsl, kSkyPsGlsl, &w.sky), makeProgram("ground", kGroundVsGlsl, kGroundPsGlsl, &w.scene3d),
                          makeProgram("particles", kParticlesVsGlsl, kParticlesPsGlsl, &w.particle),
                          makeProgram("post_bright", kPostBrightVsGlsl, kPostBrightPsGlsl, &w.bright),
                          makeProgram("post_down", kPostDownVsGlsl, kPostDownPsGlsl, &w.down),
                          makeProgram("post_up", kPostUpVsGlsl, kPostUpPsGlsl, &w.up),
                          makeProgram("post_composite", kPostCompositeVsGlsl, kPostCompositePsGlsl, &w.composite),
                          makeProgram("post_tonemap", kPostTonemapVsGlsl, kPostTonemapPsGlsl, &w.toneMap),
                          makeProgram("post_fxaa", kPostFxaaVsGlsl, kPostFxaaPsGlsl, &w.fxaa)}) {
        if (!e.empty())
            return e;
    }
    w.draws = 3 + sc::kPostPasses;
    return {};
}

void GlBackend::releaseWorkload()
{
    for (Target *t : {&m_sceneColor, &m_sceneDepth, &m_hdrB, &m_ldrB, &m_shadow})
        destroyTarget(*t);
    for (Target &t : m_bloom)
        destroyTarget(t);
    if (!m_work)
        return;
    WorkloadState &w = *m_work;
    destroyTarget(w.readTarget);
    const auto del = [](const std::vector<std::uint32_t> &ids, void (*fn)(GLsizei, const GLuint *)) {
        if (!ids.empty())
            fn(GLsizei(ids.size()), ids.data());
    };
    del(w.vertexBuffers, glDeleteBuffers);
    del(w.indexBuffers, glDeleteBuffers);
    del(w.meshVaos, glDeleteVertexArrays);
    del(w.materials, glDeleteTextures);
    del(w.fbos, glDeleteFramebuffers);
    if (w.particleBuffer)
        glDeleteBuffers(1, &w.particleBuffer);
    if (w.particleVao)
        glDeleteVertexArrays(1, &w.particleVao);
    if (w.spriteBuffer)
        glDeleteBuffers(1, &w.spriteBuffer);
    if (w.spriteVao)
        glDeleteVertexArrays(1, &w.spriteVao);
    for (const Program *p : {&w.scene3d, &w.depthOnly, &w.sky, &w.particle, &w.bright, &w.down, &w.up, &w.composite, &w.toneMap, &w.fxaa}) {
        if (p->id)
            glDeleteProgram(p->id);
    }
    m_work.reset();
}

void GlBackend::setPassState(bool depthTest, bool depthWrite, std::uint32_t depthFunc, bool additive)
{
    if (depthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    glDepthFunc(depthFunc);
    glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);
    if (additive) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glBlendEquation(GL_FUNC_ADD);
    } else {
        glDisable(GL_BLEND);
    }
    glDisable(GL_CULL_FACE);  // task 11: both faces on every API
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void GlBackend::beginPass(std::uint32_t fbo, std::uint32_t w, std::uint32_t h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, GLsizei(w), GLsizei(h));
}

// The per-object draw loop of every scene (§6.2's rule: the mesh's VAO when it
// differs from the previous draw's, the material's texture when it differs, the
// object's constants always, then the indexed draw).
std::uint32_t GlBackend::drawObjects(Slot &slot, bool materials)
{
    WorkloadState &w = *m_work;
    std::uint32_t draws = 0, lastMesh = UINT32_MAX, lastMaterial = UINT32_MAX;
    glActiveTexture(GL_TEXTURE0);
    for (std::size_t i = 0; i < w.scene.instances.size(); ++i) {
        const Instance &inst = w.scene.instances[i];
        if (inst.mesh != lastMesh) {
            glBindVertexArray(w.meshVaos[inst.mesh]);
            lastMesh = inst.mesh;
        }
        if (materials && inst.material != lastMaterial) {
            glBindTexture(GL_TEXTURE_2D, w.materials[inst.material]);
            lastMaterial = inst.material;
        }
        glBindBufferRange(GL_UNIFORM_BUFFER, 1, slot.objects, GLintptr(i * sc::kObjectSlot), GLsizeiptr(sc::kObjectSlot));
        glDrawElements(GL_TRIANGLES, GLsizei(w.indexCounts[inst.mesh]), GL_UNSIGNED_INT, nullptr);
        ++draws;
    }
    glBindVertexArray(0);
    return draws;
}

// The frame's CPU side after the slot's fence: the same bytes as Vulkan and D3D (scene_constants.h) but flipV = 1
// (Frame, Lights, the pass blocks into the CPU copy, uploaded by uploadConstants()), and the objects, mapped with
// INVALIDATE_BUFFER (the slot's previous frame is done, and the driver may rename the buffer anyway).
static void writeObjectBuffer(const SceneData &scene, double t, std::uint32_t buffer)
{
    glBindBuffer(GL_UNIFORM_BUFFER, buffer);
    void *objects = glMapBufferRange(GL_UNIFORM_BUFFER, 0, GLsizeiptr(scene.instances.size() * sc::kObjectSlot),
                                     GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (objects) {
        sc::writeObjects(scene, t, static_cast<std::uint8_t *>(objects));
        glUnmapBuffer(GL_UNIFORM_BUFFER);
    }
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void GlBackend::recordScene(Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;
    sc::writeFrame(w.scene, t, width, height, m_constants.data(), 1);
    uploadConstants(slot);
    writeObjectBuffer(w.scene, t, slot.objects);

    beginPass(w.sceneFbo, width, height);
    setPassState(true, true, GL_LESS, false);
    glClearColor(sc::kBackground[0], sc::kBackground[1], sc::kBackground[2], sc::kBackground[3]);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    useProgram(w.scene3d);
    bindFrameConstants(slot);
    m_lastDraws = drawObjects(slot, !w.materials.empty());
}

void GlBackend::recordShading(Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;
    sc::writeFrame(w.scene, t, width, height, m_constants.data(), 1);  // cameraUp.w = 1: shading.hlsl's GL-only fixes
    uploadConstants(slot);
    writeObjectBuffer(w.scene, t, slot.objects);
    std::uint32_t draws = 0;
    glClearDepth(1.0);

    // 1. The shadow pass: depth only into the shadow map (slope bias 2.0 here, the constant part in depth_only.hlsl).
    beginPass(w.shadowFbo, m_shadow.width, m_shadow.height);
    setPassState(true, true, GL_LESS, false);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 0.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    useProgram(w.depthOnly);
    bindFrameConstants(slot);
    bindPassConstants(slot, sc::blockOffset(sc::kShadowBlock), sc::kBlock);
    draws += drawObjects(slot, false);
    glDisable(GL_POLYGON_OFFSET_FILL);

    // 2. The depth pre-pass into sceneDepth (LESS, written).
    beginPass(w.prepassFbo, width, height);
    setPassState(true, true, GL_LESS, false);
    glClear(GL_DEPTH_BUFFER_BIT);
    bindPassConstants(slot, sc::blockOffset(sc::kPrepassBlock), sc::kBlock);
    draws += drawObjects(slot, false);

    // 3. The shading pass: EQUAL, no depth writes, into sceneColor (cleared); t0-t3 the textures, t4 the shadow map.
    beginPass(w.sceneFbo, width, height);
    setPassState(true, false, GL_EQUAL, false);
    glClearColor(sc::kBackground[0], sc::kBackground[1], sc::kBackground[2], sc::kBackground[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    useProgram(w.scene3d);
    bindPassConstants(slot, sc::kLightsOffset, sc::kLightsBytes);
    for (GLuint unit = 0; unit < 4; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, w.materials[unit]);
    }
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_shadow.texture);
    draws += drawObjects(slot, false);
    glActiveTexture(GL_TEXTURE0);
    m_lastDraws = draws;
}

void GlBackend::postPass(Slot &slot, const Target &target, const Program &program, std::uint32_t t0, std::uint32_t t1, std::uint32_t block,
                         bool additive)
{
    // An upsample keeps its target's contents and blends ONE+ONE; every other pass overwrites it.
    beginPass(target.fbo, target.width, target.height);
    setPassState(false, false, GL_ALWAYS, additive);
    useProgram(program);
    bindFrameConstants(slot);
    bindPassConstants(slot, sc::blockOffset(block), sc::kBlock);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t0);
    if (t1) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, t1);
        glActiveTexture(GL_TEXTURE0);
    }
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void GlBackend::recordParticles(Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;
    sc::writeFrame(w.scene, t, width, height, m_constants.data(), 1);  // flipV = 1 in the 14 Post blocks
    uploadConstants(slot);
    writeObjectBuffer(w.scene, t, slot.objects);

    // The scene pass: sky, ground, particles into sceneColor + sceneDepth (both cleared).
    beginPass(w.sceneFbo, width, height);
    setPassState(true, true, GL_LESS, false);
    glClearColor(sc::kBackground[0], sc::kBackground[1], sc::kBackground[2], sc::kBackground[3]);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    bindFrameConstants(slot);
    setPassState(true, false, GL_LEQUAL, false);  // the sky at the far plane, against the cleared 1.0
    useProgram(w.sky);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    setPassState(true, true, GL_LESS, false);
    useProgram(w.scene3d);  // geometry.hlsl's lighting plus the height fog (D2)
    std::uint32_t draws = 1 + drawObjects(slot, false);
    setPassState(true, false, GL_LEQUAL, true);  // tested, not written, ONE + ONE
    useProgram(w.particle);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, w.materials[0]);
    glBindVertexArray(w.particleVao);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, GLsizei(w.particleCount));
    glBindVertexArray(0);
    ++draws;

    // The 14 post passes (§6.4), each on a full-screen triangle (the sceneDepth isn't read: D2).
    std::uint32_t n = 0;
    postPass(slot, m_bloom[0], w.bright, m_sceneColor.texture, 0, n, false), ++n;  // 1
    for (std::size_t k = 1; k < m_bloom.size(); ++k)
        postPass(slot, m_bloom[k], w.down, m_bloom[k - 1].texture, 0, n, false), ++n;  // 2-6
    for (std::size_t k = m_bloom.size() - 1; k >= 1; --k)
        postPass(slot, m_bloom[k - 1], w.up, m_bloom[k].texture, 0, n, true), ++n;  // 7-11
    postPass(slot, m_hdrB, w.composite, m_sceneColor.texture, m_bloom[0].texture, n, false), ++n;  // 12
    postPass(slot, m_ldrA, w.toneMap, m_hdrB.texture, 0, n, false), ++n;                           // 13
    postPass(slot, m_ldrB, w.fxaa, m_ldrA.texture, 0, n, false), ++n;                              // 14
    m_lastDraws = draws + n;
}

// ---- Task 31: the fill workloads, Quads and Sprites (§6.5, §6.6) ----

std::string GlBackend::loadFill(const SceneData &scene)
{
    WorkloadState &w = *m_work;
    w.tonemap = false;  // display-encoded colour into ldrA, presented in Copy mode
    w.scene.workload = scene.workload;
    w.scene.sprites = scene.sprites;
    const bool quads = scene.workload == Workload::Quads;
    w.fillCount = quads ? scene.quadLayers : std::uint32_t(scene.sprites.size());
    if (scene.textures.size() != 1)
        return "a fill workload needs its one texture";
    std::uint32_t tex = 0;
    if (std::string e = uploadTexture(scene.textures[0], &tex); !e.empty())
        return e;
    w.materials.push_back(tex);
    if (std::string e = quads ? makeProgram("quads", kQuadsVsGlsl, kQuadsPsGlsl, &w.scene3d)
                              : makeProgram("sprites", kSpritesVsGlsl, kSpritesPsGlsl, &w.scene3d);
        !e.empty())
        return e;
    if (!quads) {
        // One buffer of every sprite, orphaned and mapped once per frame; the batches re-point the
        // instance attributes at their byte offset (GL-3: GL 3.3 has no base instance).
        glGenBuffers(1, &w.spriteBuffer);
        glGenVertexArrays(1, &w.spriteVao);
        glBindVertexArray(w.spriteVao);
        glBindBuffer(GL_ARRAY_BUFFER, w.spriteBuffer);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(std::size_t(w.fillCount) * sc::kSpriteBytes), nullptr, GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribDivisor(0, 1);
        glVertexAttribDivisor(1, 1);
        glVertexAttribDivisor(2, 1);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    if (!quads)
        w.spriteBatches = sc::spriteBatches(w.fillCount);
    w.draws = quads ? 1 : std::uint32_t(w.spriteBatches.size());
    return {};
}

std::string GlBackend::recordFill(Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const bool quads = w.workload == Workload::Quads;
    const std::uint32_t width = m_ldrA.width, height = m_ldrA.height;
    sc::writeFrame(w.scene, t, width, height, m_constants.data(), 1);
    uploadConstants(slot);
    if (!quads && w.fillCount > 0) {
        // Orphan, then map for write: the previous frames' draws keep the old storage.
        const GLsizeiptr bytes = GLsizeiptr(std::size_t(w.fillCount) * sc::kSpriteBytes);
        glBindBuffer(GL_ARRAY_BUFFER, w.spriteBuffer);
        glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_STREAM_DRAW);
        void *p = glMapBufferRange(GL_ARRAY_BUFFER, 0, bytes, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        // L1: a failed map, or an unmap whose contents were lost (GL_FALSE), fails the frame as D3D11's Map does,
        // rather than drawing undefined sprites.
        if (!p) {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            return "glMapBufferRange failed for the sprite buffer";
        }
        sc::writeSprites(w.scene, t, static_cast<std::uint8_t *>(p));
        const GLboolean unmapped = glUnmapBuffer(GL_ARRAY_BUFFER);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        if (unmapped == GL_FALSE)
            return "glUnmapBuffer lost the sprite buffer's contents";
    }
    beginPass(m_ldrA.fbo, width, height);
    setPassState(false, false, GL_ALWAYS, false);
    glClearColor(sc::kFillClear[0], sc::kFillClear[1], sc::kFillClear[2], sc::kFillClear[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    std::uint32_t draws = 0;
    if (w.fillCount > 0) {
        glEnable(GL_BLEND);  // colour SRC_ALPHA / ONE_MINUS_SRC_ALPHA, alpha ONE / ONE_MINUS_SRC_ALPHA, as every API
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);
        useProgram(w.scene3d);
        bindFrameConstants(slot);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, w.materials[0]);
        if (quads) {
            glBindVertexArray(m_vao);  // no vertex data: the corners come from gl_VertexID, the layer from gl_InstanceID
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, GLsizei(w.fillCount));
            draws = 1;
        } else {
            glBindVertexArray(w.spriteVao);
            glBindBuffer(GL_ARRAY_BUFFER, w.spriteBuffer);
            const GLsizei stride = GLsizei(sc::kSpriteBytes);
            for (const sc::SpriteBatch &b : w.spriteBatches) {
                const void *offset = reinterpret_cast<const void *>(std::uintptr_t(b.first) * sc::kSpriteBytes);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, offset);
                glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, stride, static_cast<const char *>(offset) + 8);
                glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, stride, static_cast<const char *>(offset) + 12);
                glDrawArraysInstanced(GL_TRIANGLES, 0, 6, GLsizei(b.count));
                ++draws;
            }
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
        glBindVertexArray(0);
        glDisable(GL_BLEND);
    }
    m_lastDraws = draws;
    return {};
}

} // namespace sb::gpu::gl
