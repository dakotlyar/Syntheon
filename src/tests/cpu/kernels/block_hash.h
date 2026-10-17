// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The compression sub-test's decompression check (Phase 2 design §5.4; Dave,
// 2026-10-17): each corpus block's hash is made once at prep, and every
// decompressed block's hash must equal it. In the kernels library, so its
// code is built with P7's flags like the other kernels.
//
// Eight independent lanes over little-endian 8-byte words (every SynBench
// platform is little-endian), each step h = rotl((h ^ w) * k, 31), so a bit
// of a word reaches the low bits of its lane as well as the high ones (a
// plain multiply only carries upwards: flipping bit 63 of two words of a
// lane cancelled out, AP-5). Then the tail bytes, the length, and splitmix64's
// finaliser over the lanes folded together, which mixes downwards too.
// Integers only: the same on every compiler. 14.6 us per 256 KiB block on
// Knowhere with P7's flags (four lanes: 20.4 us with the rotate, 18.3 us
// without).

#include <cstdint>
#include <span>

namespace sb::cpu {

std::uint64_t blockHash(std::span<const std::uint8_t> data);

} // namespace sb::cpu
