// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/signature.h"

namespace sb::gpu {

std::vector<std::uint8_t> blockSignature(const std::uint8_t *rgba8, std::uint32_t w, std::uint32_t h)
{
    std::vector<std::uint8_t> out;
    out.reserve(kSignatureBytes);
    for (std::uint32_t by = 0; by < std::uint32_t(kSignatureBlocksY); ++by) {
        const std::uint64_t y0 = std::uint64_t(by) * h / kSignatureBlocksY, y1 = std::uint64_t(by + 1) * h / kSignatureBlocksY;
        for (std::uint32_t bx = 0; bx < std::uint32_t(kSignatureBlocksX); ++bx) {
            const std::uint64_t x0 = std::uint64_t(bx) * w / kSignatureBlocksX, x1 = std::uint64_t(bx + 1) * w / kSignatureBlocksX;
            std::uint64_t sum[3] = {0, 0, 0};
            const std::uint64_t n = (x1 - x0) * (y1 - y0);
            for (std::uint64_t y = y0; y < y1; ++y) {
                const std::uint8_t *row = rgba8 + (y * w + x0) * 4;
                for (std::uint64_t x = x0; x < x1; ++x, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                }
            }
            for (const std::uint64_t s : sum)
                out.push_back(n ? std::uint8_t((s + n / 2) / n) : std::uint8_t(0));
        }
    }
    return out;
}

} // namespace sb::gpu
