// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Procedural textures (Phase 4a design §6): no image assets. Everything is
// computed with + - * / and sqrt only (no exp, pow or trigonometry), so the
// bytes are identical on every compiler and OS.

#include "tests/gpu/worker/scene/scenes.h"

namespace sb::gpu {

Texture noiseAlbedo(std::uint32_t seed, std::uint32_t size);
Texture noiseNormal(std::uint32_t seed, std::uint32_t size);  // from a noise height, Sobel
Texture roughMetal(std::uint32_t seed, std::uint32_t size);   // R roughness 0.2-0.9, G metal 0 or 1 in bands
Texture aoMap(std::uint32_t seed, std::uint32_t size);
Texture checker(std::uint32_t seed, std::uint32_t size);      // checker or stripes, by seed
Texture softDisc(std::uint32_t size);                         // white, alpha = a soft falloff
Texture spriteAtlas(std::uint32_t seed);                      // 256 x 256: 4 x 4 cells of 64 x 64 shapes
Texture fillTexture(std::uint32_t seed);                      // 256 x 256 (the quads workload)
Texture blitImage(std::uint32_t seed);                        // 256 x 256 (native 2D image blits, §7.1)
// Box filter, level 0 first, down to 1 x 1 (floor(log2(max side)) + 1 levels).
std::vector<Texture> mipChain(const Texture &level0);

} // namespace sb::gpu
