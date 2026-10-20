// SPDX-License-Identifier: GPL-3.0-or-later
// The SSE2 bandwidth path (Phase 3 design §5.3): 128-bit loads and stores,
// 8 registers (128 bytes) per iteration; every x64 CPU has it. Baseline
// flags, but it follows bw_common.h's ODR rule like the wide TUs (the
// simd_odr CTest checks it too): only <emmintrin.h> and bw_common.h, only
// its sb_bw_sse2_* entry points.
#include "tests/ram/kernels/bw/bw_common.h"

#include <emmintrin.h>

namespace sb::ram::bw_sse2 {
namespace {

constexpr std::size_t kRow = 128;  // 8 x 16 bytes

__m128i lanes(std::uint64_t first)
{
    return _mm_set_epi64x(static_cast<long long>(first + 1), static_cast<long long>(first));
}

} // namespace
} // namespace sb::ram::bw_sse2

using namespace sb::ram::bw_sse2;

// The two store variants are one macro each, not a template: the loop must
// sit in the entry point itself, where the bw_stores CTest looks for (or
// rules out) the non-temporal stores; a template instance would be a
// separate local function the entry point jumps to.

extern "C" std::uint64_t sb_bw_sse2_read(const void *p, std::size_t bytes)
{
    const char *at = static_cast<const char *>(p);
    const char *const end = at + bytes;
    __m128i a0 = _mm_setzero_si128(), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
    for (; at < end; at += kRow) {
        a0 = _mm_add_epi64(a0, _mm_load_si128(reinterpret_cast<const __m128i *>(at)));
        a1 = _mm_add_epi64(a1, _mm_load_si128(reinterpret_cast<const __m128i *>(at + 16)));
        a2 = _mm_add_epi64(a2, _mm_load_si128(reinterpret_cast<const __m128i *>(at + 32)));
        a3 = _mm_add_epi64(a3, _mm_load_si128(reinterpret_cast<const __m128i *>(at + 48)));
        a4 = _mm_add_epi64(a4, _mm_load_si128(reinterpret_cast<const __m128i *>(at + 64)));
        a5 = _mm_add_epi64(a5, _mm_load_si128(reinterpret_cast<const __m128i *>(at + 80)));
        a6 = _mm_add_epi64(a6, _mm_load_si128(reinterpret_cast<const __m128i *>(at + 96)));
        a7 = _mm_add_epi64(a7, _mm_load_si128(reinterpret_cast<const __m128i *>(at + 112)));
    }
    const __m128i h = _mm_add_epi64(_mm_add_epi64(_mm_add_epi64(a0, a1), _mm_add_epi64(a2, a3)),
                                    _mm_add_epi64(_mm_add_epi64(a4, a5), _mm_add_epi64(a6, a7)));
    return std::uint64_t(_mm_cvtsi128_si64(h)) + std::uint64_t(_mm_cvtsi128_si64(_mm_unpackhi_epi64(h, h)));
}

#define SB_BW_SSE2_WRITE(name, store, fence)                                                                         \
    extern "C" void name(void *p, std::size_t bytes, std::uint64_t base)                                             \
    {                                                                                                                \
        char *at = static_cast<char *>(p);                                                                           \
        char *const end = at + bytes;                                                                                \
        __m128i v0 = lanes(base), v1 = lanes(base + 2), v2 = lanes(base + 4), v3 = lanes(base + 6);                  \
        __m128i v4 = lanes(base + 8), v5 = lanes(base + 10), v6 = lanes(base + 12), v7 = lanes(base + 14);           \
        const __m128i step = _mm_set1_epi64x(16);                                                                    \
        for (; at < end; at += kRow) {                                                                               \
            store(reinterpret_cast<__m128i *>(at), v0);                                                              \
            store(reinterpret_cast<__m128i *>(at + 16), v1);                                                         \
            store(reinterpret_cast<__m128i *>(at + 32), v2);                                                         \
            store(reinterpret_cast<__m128i *>(at + 48), v3);                                                         \
            store(reinterpret_cast<__m128i *>(at + 64), v4);                                                         \
            store(reinterpret_cast<__m128i *>(at + 80), v5);                                                         \
            store(reinterpret_cast<__m128i *>(at + 96), v6);                                                         \
            store(reinterpret_cast<__m128i *>(at + 112), v7);                                                        \
            v0 = _mm_add_epi64(v0, step);                                                                            \
            v1 = _mm_add_epi64(v1, step);                                                                            \
            v2 = _mm_add_epi64(v2, step);                                                                            \
            v3 = _mm_add_epi64(v3, step);                                                                            \
            v4 = _mm_add_epi64(v4, step);                                                                            \
            v5 = _mm_add_epi64(v5, step);                                                                            \
            v6 = _mm_add_epi64(v6, step);                                                                            \
            v7 = _mm_add_epi64(v7, step);                                                                            \
        }                                                                                                            \
        fence;                                                                                                       \
    }

#define SB_BW_SSE2_COPY(name, store, fence)                                                                          \
    extern "C" void name(void *dst, const void *src, std::size_t bytes)                                              \
    {                                                                                                                \
        char *to = static_cast<char *>(dst);                                                                         \
        const char *from = static_cast<const char *>(src);                                                           \
        const char *const end = from + bytes;                                                                        \
        for (; from < end; from += kRow, to += kRow) {                                                               \
            const __m128i v0 = _mm_load_si128(reinterpret_cast<const __m128i *>(from));                              \
            const __m128i v1 = _mm_load_si128(reinterpret_cast<const __m128i *>(from + 16));                         \
            const __m128i v2 = _mm_load_si128(reinterpret_cast<const __m128i *>(from + 32));                         \
            const __m128i v3 = _mm_load_si128(reinterpret_cast<const __m128i *>(from + 48));                         \
            const __m128i v4 = _mm_load_si128(reinterpret_cast<const __m128i *>(from + 64));                         \
            const __m128i v5 = _mm_load_si128(reinterpret_cast<const __m128i *>(from + 80));                         \
            const __m128i v6 = _mm_load_si128(reinterpret_cast<const __m128i *>(from + 96));                         \
            const __m128i v7 = _mm_load_si128(reinterpret_cast<const __m128i *>(from + 112));                        \
            store(reinterpret_cast<__m128i *>(to), v0);                                                              \
            store(reinterpret_cast<__m128i *>(to + 16), v1);                                                         \
            store(reinterpret_cast<__m128i *>(to + 32), v2);                                                         \
            store(reinterpret_cast<__m128i *>(to + 48), v3);                                                         \
            store(reinterpret_cast<__m128i *>(to + 64), v4);                                                         \
            store(reinterpret_cast<__m128i *>(to + 80), v5);                                                         \
            store(reinterpret_cast<__m128i *>(to + 96), v6);                                                         \
            store(reinterpret_cast<__m128i *>(to + 112), v7);                                                        \
        }                                                                                                            \
        fence;                                                                                                       \
    }

SB_BW_SSE2_WRITE(sb_bw_sse2_write, _mm_store_si128, (void)0)
SB_BW_SSE2_WRITE(sb_bw_sse2_write_nt, _mm_stream_si128, _mm_sfence())
SB_BW_SSE2_COPY(sb_bw_sse2_copy, _mm_store_si128, (void)0)
SB_BW_SSE2_COPY(sb_bw_sse2_copy_nt, _mm_stream_si128, _mm_sfence())
