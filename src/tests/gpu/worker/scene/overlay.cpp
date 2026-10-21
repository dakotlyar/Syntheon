// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/scene/overlay.h"

#include "tests/gpu/worker/scene/bitmap_font.h"

#include <algorithm>
#include <cmath>

namespace sb::gpu {

namespace {
constexpr std::size_t kMaxChars = 40;

int glyphIndex(char c)
{
    const int code = static_cast<unsigned char>(c);
    return code >= kFirstChar && code < kFirstChar + kGlyphCount ? code - kFirstChar : '?' - kFirstChar;
}
} // namespace

std::string overlayText(std::string_view scene, std::string_view api, std::optional<int> fps)
{
    std::string text;
    text.append(scene).append(" | ").append(api).append(" | ");
    text += fps ? std::to_string(*fps) + " fps" : std::string("warming up");
    for (char &c : text) {
        const int code = static_cast<unsigned char>(c);
        if (code < kFirstChar || code >= kFirstChar + kGlyphCount)
            c = '?';
    }
    if (text.size() > kMaxChars)
        text.resize(kMaxChars);
    return text;
}

OverlayLayout overlayLayout(std::size_t chars, double scale)
{
    OverlayLayout l;
    l.k = std::max(1, int(std::lround(2.0 * scale)));
    l.x = l.y = int(std::lround(8.0 * scale));
    l.pad = 3 * l.k;
    l.w = int(chars) * 6 * l.k - l.k + 2 * l.pad;
    l.h = 7 * l.k + 2 * l.pad;
    return l;
}

std::array<std::uint32_t, 12> packOverlay(std::string_view text)
{
    std::array<std::uint32_t, 12> out{};
    const std::size_t n = std::min<std::size_t>(text.size(), 48);
    for (std::size_t i = 0; i < n; ++i)
        out[i / 4] |= std::uint32_t(static_cast<unsigned char>(text[i])) << (8 * (i % 4));
    return out;
}

std::vector<std::uint8_t> rasterOverlay(std::string_view text, double scale, OverlayLayout *layout)
{
    const OverlayLayout l = overlayLayout(text.size(), scale);
    if (layout)
        *layout = l;
    std::vector<std::uint8_t> px(std::size_t(l.w) * std::size_t(l.h) * 4);
    for (int y = 0; y < l.h; ++y) {
        for (int x = 0; x < l.w; ++x) {
            bool ink = false;
            if (x >= l.pad && y >= l.pad) {
                const int lx = (x - l.pad) / l.k, ly = (y - l.pad) / l.k;
                const std::size_t c = std::size_t(lx / kCellW);
                const int gx = lx % kCellW;
                if (c < text.size() && gx < kGlyphW && ly < kGlyphH)
                    ink = (kGlyphRows[glyphIndex(text[c])][ly] & (0x10 >> gx)) != 0;
            }
            std::uint8_t *p = &px[(std::size_t(y) * std::size_t(l.w) + std::size_t(x)) * 4];
            p[0] = p[1] = p[2] = ink ? 255 : 0;
            p[3] = ink ? 255 : 153;
        }
    }
    return px;
}

} // namespace sb::gpu
