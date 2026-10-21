// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The verification frame's colour signature (Phase 4a design §5.7, P14).
// Qt-free.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sb::gpu {

inline constexpr int kSignatureBlocksX = 16, kSignatureBlocksY = 9;
inline constexpr std::size_t kSignatureBytes = std::size_t(kSignatureBlocksX) * kSignatureBlocksY * 3;  // 432

// 16x9 block means of an RGBA8 image (top row first), RGB only, blocks in
// row-major order: 432 bytes. Block (bx, by) covers x in [bx*w/16,
// (bx+1)*w/16) and y in [by*h/9, (by+1)*h/9) (integer division); each mean is
// rounded as (sum + n/2) / n. An image smaller than 16x9 gives an empty block
// a mean of 0.
std::vector<std::uint8_t> blockSignature(const std::uint8_t *rgba8, std::uint32_t w, std::uint32_t h);

} // namespace sb::gpu
