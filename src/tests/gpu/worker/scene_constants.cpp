// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/scene_constants.h"

#include <cstring>

namespace sb::gpu::sc {

namespace {

// The scenes' Frame constants (common.hlsli): the camera at t, a sun, the hemisphere ambient.
FrameConstants frameConstants(const Camera &cam, double t, Vec3 sunTravel, Vec3 sunColour, std::uint32_t width, std::uint32_t height)
{
    FrameConstants fc{};
    const Mat4 viewProj = cam.proj * cam.view;
    std::memcpy(fc.viewProj, viewProj.m, sizeof fc.viewProj);
    const Mat4 identity = Mat4::identity();
    std::memcpy(fc.lightViewProj, identity.m, sizeof fc.lightViewProj);
    const float eye[4] = {cam.eye.x, cam.eye.y, cam.eye.z, float(t)};  // w = the animation time
    std::memcpy(fc.cameraPos, eye, sizeof eye);
    const float right[4] = {cam.view.m[0], cam.view.m[4], cam.view.m[8], 0.f};  // the view matrix's rows
    const float up[4] = {cam.view.m[1], cam.view.m[5], cam.view.m[9], 0.f};
    std::memcpy(fc.cameraRight, right, sizeof right);
    std::memcpy(fc.cameraUp, up, sizeof up);
    const Vec3 sun = normalize(sunTravel);
    const float sunDir[4] = {sun.x, sun.y, sun.z, 0.f}, colour[4] = {sunColour.x, sunColour.y, sunColour.z, 0.f};
    const float sky[4] = {0.25f, 0.30f, 0.40f, 0.f}, ground[4] = {0.08f, 0.07f, 0.06f, 0.f};
    std::memcpy(fc.sunDir, sunDir, sizeof sunDir);
    std::memcpy(fc.sunColour, colour, sizeof colour);
    std::memcpy(fc.skyAmbient, sky, sizeof sky);
    std::memcpy(fc.groundAmbient, ground, sizeof ground);
    const float size[4] = {float(width), float(height), 1.f / float(width), 1.f / float(height)};
    std::memcpy(fc.renderSize, size, sizeof size);
    return fc;
}

void writePost(std::uint8_t *constants, std::uint32_t index, std::uint32_t srcW, std::uint32_t srcH, float p0, float p1, float p2,
               std::uint32_t flipV)
{
    PostConstants pc{};
    const float size[4] = {1.f / float(srcW), 1.f / float(srcH), float(srcW), float(srcH)};
    const float params[4] = {p0, p1, p2, 0.f};
    std::memcpy(pc.sourceSize, size, sizeof size);
    std::memcpy(pc.params, params, sizeof params);
    pc.flags[0] = flipV;
    std::memcpy(constants + blockOffset(index), &pc, sizeof pc);
}

} // namespace

std::uint32_t bloomSize(std::uint32_t size, std::uint32_t level)
{
    const std::uint32_t div = 2u << level;
    const std::uint32_t s = (size + div - 1) / div;
    return s > 0 ? s : 1;
}

Mat4 sunViewProj(Vec3 sunTravel)
{
    const Vec3 d = normalize(sunTravel);
    const Vec3 centre{0.f, 4.f, 0.f};
    const Mat4 view = lookAt(centre - d * 40.f, centre, {0.f, 0.f, 1.f});
    return orthographic(-22.f, 22.f, -22.f, 22.f, 1.f, 80.f) * view;
}

void writeObjects(const SceneData &scene, double t, std::uint8_t *objects)
{
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        ObjectConstants oc{};
        const Mat4 model = instanceModelAt(scene, i, t);
        std::memcpy(oc.model, model.m, sizeof oc.model);
        const Vec4 &c = scene.instances[i].colour;
        const float colour[4] = {c.x, c.y, c.z, c.w};
        std::memcpy(oc.colour, colour, sizeof colour);
        std::memcpy(objects + i * kObjectSlot, &oc, sizeof oc);
    }
}

void writeFrame(const SceneData &scene, double t, std::uint32_t width, std::uint32_t height, std::uint8_t *constants, std::uint32_t flipV)
{
    const double aspect = double(width) / double(height);
    const Camera cam = cameraAt(scene.workload, t, aspect);
    switch (scene.workload) {
    case Workload::Geometry:
    case Workload::DrawCalls: {
        // §6.1 (and §6.2, "as in Geometry"): the sun normalise(-0.4, -1, -0.3), (1, 0.96, 0.90) x 3.
        const FrameConstants fc = frameConstants(cam, t, {-0.4f, -1.f, -0.3f}, Vec3{1.f, 0.96f, 0.90f} * 3.f, width, height);
        std::memcpy(constants, &fc, sizeof fc);
        return;
    }
    case Workload::Shading: {
        // §6.3: the sun normalise(-0.3, -1, -0.5), (1, 0.95, 0.9) x 4, its shadow matrix,
        // the light count in sunDir.w; the 128-light block; the DepthPass flags.
        const Vec3 sunTravel{-0.3f, -1.f, -0.5f};
        FrameConstants fc = frameConstants(cam, t, sunTravel, Vec3{1.f, 0.95f, 0.9f} * 4.f, width, height);
        const Mat4 lvp = sunViewProj(sunTravel);
        std::memcpy(fc.lightViewProj, lvp.m, sizeof fc.lightViewProj);
        fc.sunDir[3] = float(scene.lights.size());
        std::memcpy(constants, &fc, sizeof fc);
        LightsConstants lights{};
        const std::vector<PointLight> now = lightsAt(scene, t);
        for (std::size_t k = 0; k < now.size() && k < kPointLights; ++k) {
            const PointLight &l = now[k];
            const float pr[4] = {l.position.x, l.position.y, l.position.z, l.radius};
            const float ci[4] = {l.colour.x, l.colour.y, l.colour.z, l.intensity};
            std::memcpy(lights.posRadius[k], pr, sizeof pr);
            std::memcpy(lights.colourIntensity[k], ci, sizeof ci);
        }
        std::memcpy(constants + kLightsOffset, &lights, sizeof lights);
        const std::uint32_t shadowFlag[4] = {1, 0, 0, 0}, prepassFlag[4] = {0, 0, 0, 0};
        std::memcpy(constants + blockOffset(kShadowBlock), shadowFlag, sizeof shadowFlag);
        std::memcpy(constants + blockOffset(kPrepassBlock), prepassFlag, sizeof prepassFlag);
        return;
    }
    case Workload::Particles: {
        // §6.4: the Geometry sun lights the ground; fog.hlsli's height fog (D2):
        // density 0.02 in skyAmbient.w, height falloff 0.15 in groundAmbient.w.
        FrameConstants fc = frameConstants(cam, t, {-0.4f, -1.f, -0.3f}, Vec3{1.f, 0.96f, 0.90f} * 3.f, width, height);
        fc.skyAmbient[3] = 0.02f;
        fc.groundAmbient[3] = 0.15f;
        std::memcpy(constants, &fc, sizeof fc);
        // The 14 Post blocks, in pass order: bright, down 1-5, up 5->4 .. 1->0, composite, tone map, FXAA.
        std::uint32_t b = 0;
        writePost(constants, b++, width, height, 1.0f, 0.5f, 0.f, flipV);  // bright: threshold 1.0, soft knee 0.5
        for (std::uint32_t k = 1; k < kBloomLevels; ++k)
            writePost(constants, b++, bloomSize(width, k - 1), bloomSize(height, k - 1), 0.f, 0.f, 0.f, flipV);
        for (std::uint32_t k = kBloomLevels - 1; k >= 1; --k)
            writePost(constants, b++, bloomSize(width, k), bloomSize(height, k), 1.0f, 0.f, 0.f, flipV);  // tent radius 1 texel
        writePost(constants, b++, width, height, 0.04f, 0.f, 0.f, flipV);  // composite: bloom 0.04 (the fog is the surfaces', D2)
        writePost(constants, b++, width, height, 0.04f, 0.f, 0.f, flipV);  // tone map: grain amplitude
        writePost(constants, b++, width, height, 0.f, 0.f, 0.f, flipV);    // FXAA
        return;
    }
    case Workload::Quads:
    case Workload::Sprites: return;  // task 29
    }
}

} // namespace sb::gpu::sc
