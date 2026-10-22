// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The CPU side of the 3D workloads, shared by every 3D backend so the shaders
// get the same bytes on each API (Phase 4a design §6, D20; tasks 11, 12, 19):
// the cbuffer layouts of common.hlsli / shading.hlsl / post.hlsli, where the
// pass blocks sit in a slot's frame-constants buffer, and the values each
// scene writes per frame. API-free and Qt-free.

#include "tests/gpu/worker/scene/scenes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sb::gpu::sc {

// A slot's frame-constants buffer (64 KiB): Frame at 0, Present at 256, Lights
// at 512 (4 KiB), then 256-byte blocks from 4608 (Shading: DepthPass shadow /
// pre-pass; Particles: the 14 Post blocks). Every offset is 256-aligned (DX-4).
inline constexpr std::size_t kFrameConstantsBytes = 64 * 1024, kBlock = 256, kPresentOffset = 256, kLightsOffset = 512,
                             kLightsBytes = 4096, kBlockBase = kLightsOffset + kLightsBytes;
inline constexpr std::uint32_t kShadowBlock = 0, kPrepassBlock = 1, kPostPasses = 14;
static_assert(kBlockBase + kPostPasses * kBlock + kLightsBytes <= kFrameConstantsBytes, "the pass blocks fit the buffer");
inline constexpr std::size_t blockOffset(std::uint32_t block) { return kBlockBase + block * kBlock; }

// One 256-byte slot per object in the objects buffer (b1, a per-draw offset/address).
inline constexpr std::size_t kObjectSlot = 256;
inline constexpr std::uint32_t kShadowSize = 2048;                 // §6.3: the shadow map, D32F
inline constexpr float kBackground[4] = {0.02f, 0.02f, 0.03f, 1.0f};  // sceneColor's clear (task 11)
inline constexpr std::uint32_t kBloomLevels = 6;
// The fill workloads (Quads, Sprites) clear ldrA to this each frame (the colour the clear-only workload of tasks 10, 18 and 21 used: (64, 128, 192, 255)).
inline constexpr float kFillClear[4] = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};

// common.hlsli's cbuffer Frame (b0).
struct FrameConstants {
    float viewProj[16], lightViewProj[16];
    float cameraPos[4], cameraRight[4], cameraUp[4];
    float sunDir[4], sunColour[4], skyAmbient[4], groundAmbient[4];
    float renderSize[4];
};
static_assert(sizeof(FrameConstants) == kBlock, "the Frame cbuffer is 256 bytes");

// cbuffer Object (b1) of geometry/drawcalls/ground/shading/depth_only.
struct ObjectConstants {
    float model[16];
    float colour[4];
};
static_assert(sizeof(ObjectConstants) <= kObjectSlot, "an object's constants fit its slot");

// shading.hlsl's cbuffer Lights (b2): 128 lights x 32 bytes.
struct LightsConstants {
    float posRadius[kPointLights][4];
    float colourIntensity[kPointLights][4];
};
static_assert(sizeof(LightsConstants) == kLightsBytes, "the Lights cbuffer is 4 KiB");

// post.hlsli's cbuffer Post (b2).
struct PostConstants {
    float sourceSize[4];  // 1/w, 1/h, w, h of t0
    float params[4];
    std::uint32_t flags[4];  // x = flipV (OpenGL only)
};

// §5.3: bloom[k] is ceil(size / 2^(k+1)), never 0 (VK-6).
std::uint32_t bloomSize(std::uint32_t size, std::uint32_t level);

// Everything a 3D scene writes into a slot's frame constants for time t at a
// render size of width x height (Frame, plus Lights and the DepthPass flags for
// Shading, plus the fog and the 14 Post blocks for Particles), and every
// object's slot into `objects`. flipV goes into the Post blocks (OpenGL).
void writeFrame(const SceneData &scene, double t, std::uint32_t width, std::uint32_t height, std::uint8_t *constants,
                std::uint32_t flipV = 0);
void writeObjects(const SceneData &scene, double t, std::uint8_t *objects);

// §6.5, §6.6 (tasks 29-31): the fill workloads. A sprite instance is 16 bytes: {float x, y; uint32 rotation16 | atlasIndex << 16;
// uint32 rgba8}; writeSprites writes every sprite's position at time t, x = mod(x0 + vx t, 1920), y = mod(y0 + vy t, 1080)
// (logical px), into `out` (sprites x 16 bytes). The sprites are drawn in batches of up to kSpriteBatch instances.
inline constexpr std::size_t kSpriteBytes = 16;
void writeSprites(const SceneData &scene, double t, std::uint8_t *out);
struct SpriteBatch { std::uint32_t first, count; };
// 100,000 sprites in batches of 4,096 -> 25 batches, the last of 1,696.
std::vector<SpriteBatch> spriteBatches(std::uint32_t sprites, std::uint32_t batch = kSpriteBatch);

// §6.3: the sun's shadow matrix (also written into Frame::lightViewProj for Shading).
Mat4 sunViewProj(Vec3 sunTravel);

} // namespace sb::gpu::sc
