// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// present.hlsl's cbuffer Present (b2; 80 bytes, std140 = the HLSL packing),
// filled the same way by every 3D backend (Phase 4a design §5.3, §5.9, task 7's
// layout). Qt-free.

#include "tests/gpu/worker/scene/overlay.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace sb::gpu {

struct PresentConstants {
    std::uint32_t mode;   // kPresentTonemap or kPresentCopy
    std::uint32_t flipV;  // 1 on OpenGL only
    std::int32_t overlayScale, overlayChars;
    std::int32_t overlayBox[4];  // x, y, w, h in window physical px (OpenGL: y from the bottom, task 7)
    std::uint32_t overlayText[12];
};
static_assert(sizeof(PresentConstants) == 80, "the Present cbuffer is 80 bytes");

inline constexpr std::uint32_t kPresentTonemap = 0, kPresentCopy = 1;

// The constants for one present pass into a window of windowW x windowH at
// `scale`; an overlay that doesn't fit the window is left out entirely (never a
// partial box). Top-left origin (D3D, Vulkan).
inline PresentConstants presentConstants(std::uint32_t mode, const std::string &overlay, double scale, std::uint32_t windowW,
                                         std::uint32_t windowH)
{
    PresentConstants c{};
    c.mode = mode;
    c.flipV = 0;
    if (!overlay.empty()) {
        const OverlayLayout l = overlayLayout(overlay.size(), scale);
        if (std::uint32_t(l.x + l.w) <= windowW && std::uint32_t(l.y + l.h) <= windowH) {
            c.overlayScale = l.k;
            c.overlayChars = std::int32_t(overlay.size());
            c.overlayBox[0] = l.x;
            c.overlayBox[1] = l.y;
            c.overlayBox[2] = l.w;
            c.overlayBox[3] = l.h;
            const std::array<std::uint32_t, 12> packed = packOverlay(overlay);
            std::memcpy(c.overlayText, packed.data(), sizeof c.overlayText);
        }
    }
    return c;
}

} // namespace sb::gpu
