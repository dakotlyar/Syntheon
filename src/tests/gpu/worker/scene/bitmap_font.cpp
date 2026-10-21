// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/scene/bitmap_font.h"

namespace sb::gpu {

const std::uint8_t kGlyphRows[kGlyphCount][kGlyphH] = {
#include "tests/gpu/worker/scene/bitmap_font.inc"
};

Texture glyphAtlas()
{
    Texture t;
    t.width = std::uint32_t(kGlyphCount * kCellW);
    t.height = std::uint32_t(kCellH);
    t.mips = false;
    t.rgba8.assign(std::size_t(t.width) * t.height * 4, 0);
    for (int g = 0; g < kGlyphCount; ++g) {
        for (int row = 0; row < kGlyphH; ++row) {
            for (int col = 0; col < kGlyphW; ++col) {
                if (!(kGlyphRows[g][row] & (0x10 >> col)))
                    continue;
                std::uint8_t *p = &t.rgba8[(std::size_t(row) * t.width + std::size_t(g * kCellW + col)) * 4];
                p[0] = p[1] = p[2] = p[3] = 255;
            }
        }
    }
    return t;
}

} // namespace sb::gpu
