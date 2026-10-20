// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the bandwidth kernels' per-ISA TUs share with the rest of the build
// (Phase 3 design §5.3, P8). Phase 2's ODR rule (simd_common.h) holds here
// too: a TU compiled with wider arch flags than the baseline (bw_avx2.cpp)
// includes only its intrinsics header and this file, which holds constants
// and declarations: no inline functions, no templates. Each per-ISA TU keeps
// its code in namespace sb::ram::bw_<isa> (helpers in an anonymous namespace
// inside it) and exports only its extern "C" entry points sb_bw_<isa>_*; the
// simd_odr CTest checks the objects ("bw:" entries).
//
// Every entry point takes a 64-byte-aligned region whose length is a
// multiple of kBwChunk, and works on it with 8 independent registers:
//   read      64-bit integer adds of every 64-bit word into 8 accumulators;
//             returns the sum of all words, mod 2^64 (exact: integer adds
//             in any order give the same sum)
//   write     word i of the region = base + i (a per-unit pattern, so a
//             sampled line shows whether its stores landed where they should)
//   copy      dst = src
// x64 has two store variants for write and copy (D4): plain (temporal)
// stores, and non-temporal ones (_nt: MOVNTDQ / VMOVNTDQ) followed by an
// SFENCE, so the stores have drained before the unit's time is taken. ARM64
// has plain stores only (§5.3: STNP is only a hint). The bw_stores CTest
// checks the instructions in the objects: _nt functions use non-temporal
// stores and SFENCE, the plain ones don't, and none of them calls anything
// (a compiler turning a copy loop into memcpy could get NT stores from the
// C library).

#include <cstddef>
#include <cstdint>

namespace sb::ram::bw {

// Namespace-scope constexpr: internal linkage, so no symbol in any object.
constexpr std::size_t kBwChunk = 512;  // 8 registers x 64 bytes: AVX2 does two such rows per 512
constexpr std::size_t kBwAlign = 64;

} // namespace sb::ram::bw

extern "C" {
std::uint64_t sb_bw_sse2_read(const void *p, std::size_t bytes);
void sb_bw_sse2_write(void *p, std::size_t bytes, std::uint64_t base);
void sb_bw_sse2_write_nt(void *p, std::size_t bytes, std::uint64_t base);
void sb_bw_sse2_copy(void *dst, const void *src, std::size_t bytes);
void sb_bw_sse2_copy_nt(void *dst, const void *src, std::size_t bytes);

std::uint64_t sb_bw_avx2_read(const void *p, std::size_t bytes);
void sb_bw_avx2_write(void *p, std::size_t bytes, std::uint64_t base);
void sb_bw_avx2_write_nt(void *p, std::size_t bytes, std::uint64_t base);
void sb_bw_avx2_copy(void *dst, const void *src, std::size_t bytes);
void sb_bw_avx2_copy_nt(void *dst, const void *src, std::size_t bytes);

std::uint64_t sb_bw_neon_read(const void *p, std::size_t bytes);
void sb_bw_neon_write(void *p, std::size_t bytes, std::uint64_t base);
void sb_bw_neon_copy(void *dst, const void *src, std::size_t bytes);
}
