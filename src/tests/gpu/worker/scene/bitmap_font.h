// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The overlay's built-in 5x7 bitmap font (Phase 4a design §5.9): ASCII 32..126
// from X.Org's misc-fixed 5x7 (public domain), converted by
// tools/gpu-shaders/font5x7.py into bitmap_font.inc.

#include "tests/gpu/worker/scene/scenes.h"

#include <cstdint>

namespace sb::gpu {

inline constexpr int kGlyphW = 5, kGlyphH = 7, kCellW = 6, kCellH = 9, kFirstChar = 32, kGlyphCount = 95;
// kGlyphRows[c - 32][row]: 5 bits, bit 4 = the leftmost pixel; row 0 = the top.
extern const std::uint8_t kGlyphRows[kGlyphCount][kGlyphH];
// 570 x 9 RGBA8: glyph g at x = 6g, rows 0-6; ink = (255,255,255,255), else 0.
Texture glyphAtlas();

} // namespace sb::gpu
