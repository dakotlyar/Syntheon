// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The FPS and scene overlay's text and geometry (Phase 4a design §5.9, M12),
// shared by the present shader's constants and the native 2D paths' CPU raster.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sb::gpu {

// "Geometry | Vulkan | 412 fps", or "... | warming up"; ASCII only, cut to 40 characters.
std::string overlayText(std::string_view scene, std::string_view api, std::optional<int> fps);

struct OverlayLayout {
    int x = 0, y = 0, w = 0, h = 0, k = 1, pad = 3;  // window physical px
};
// k = max(1, round(2 s)); margin m = round(8 s) (x = y = m); pad = 3k;
// w = chars x 6k - k + 2 pad; h = 7k + 2 pad.
OverlayLayout overlayLayout(std::size_t chars, double scale);
// 4 characters per uint, little-endian, 0 after the end (the Present cbuffer's overlayText[3]).
std::array<std::uint32_t, 12> packOverlay(std::string_view text);
// The same box pixels the present shader draws, for the native paths: RGBA8
// premultiplied, background (0,0,0,153), ink (255,255,255,255); layout.w x layout.h.
std::vector<std::uint8_t> rasterOverlay(std::string_view text, double scale, OverlayLayout *layout);

} // namespace sb::gpu
