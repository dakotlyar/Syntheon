// SPDX-License-Identifier: GPL-3.0-or-later
// The AVX2 bandwidth path (Phase 3 design §5.3): 256-bit loads and stores,
// 8 registers (256 bytes) per iteration. A wide TU (-mavx2 / /arch:AVX2):
// it follows the ODR rule in bw_common.h, so it includes only <immintrin.h>
// and bw_common.h and exports only its sb_bw_avx2_* entry points.
#include "tests/ram/kernels/bw/bw_common.h"

#include <immintrin.h>

namespace sb::ram::bw_avx2 {
namespace {

constexpr std::size_t kRow = 256;  // 8 x 32 bytes

__m256i lanes(std::uint64_t first)
{
    return _mm256_set_epi64x(static_cast<long long>(first + 3), static_cast<long long>(first + 2),
                             static_cast<long long>(first + 1), static_cast<long long>(first));
}

} // namespace
} // namespace sb::ram::bw_avx2

using namespace sb::ram::bw_avx2;

// The two store variants are one macro each, not a template: the loop must
// sit in the entry point itself, where the bw_stores CTest looks for (or
// rules out) the non-temporal stores; a template instance would be a
// separate local function the entry point jumps to.

extern "C" std::uint64_t sb_bw_avx2_read(const void *p, std::size_t bytes)
{
    const char *at = static_cast<const char *>(p);
    const char *const end = at + bytes;
    __m256i a0 = _mm256_setzero_si256(), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
    for (; at < end; at += kRow) {
        a0 = _mm256_add_epi64(a0, _mm256_load_si256(reinterpret_cast<const __m256i *>(at)));
        a1 = _mm256_add_epi64(a1, _mm256_load_si256(reinterpret_cast<const __m256i *>(at + 32)));
        a2 = _mm256_add_epi64(a2, _mm256_load_si256(reinterpret_cast<const __m256i *>(at + 64)));
        a3 = _mm256_add_epi64(a3, _mm256_load_si256(reinterpret_cast<const __m256i *>(at + 96)));
        a4 = _mm256_add_epi64(a4, _mm256_load_si256(reinterpret_cast<const __m256i *>(at + 128)));
        a5 = _mm256_add_epi64(a5, _mm256_load_si256(reinterpret_cast<const __m256i *>(at + 160)));
        a6 = _mm256_add_epi64(a6, _mm256_load_si256(reinterpret_cast<const __m256i *>(at + 192)));
        a7 = _mm256_add_epi64(a7, _mm256_load_si256(reinterpret_cast<const __m256i *>(at + 224)));
    }
    const __m256i s = _mm256_add_epi64(_mm256_add_epi64(_mm256_add_epi64(a0, a1), _mm256_add_epi64(a2, a3)),
                                       _mm256_add_epi64(_mm256_add_epi64(a4, a5), _mm256_add_epi64(a6, a7)));
    const __m128i h = _mm_add_epi64(_mm256_castsi256_si128(s), _mm256_extracti128_si256(s, 1));
    return std::uint64_t(_mm_cvtsi128_si64(h)) + std::uint64_t(_mm_cvtsi128_si64(_mm_unpackhi_epi64(h, h)));
}

#define SB_BW_AVX2_WRITE(name, store, fence)                                                                         \
    extern "C" void name(void *p, std::size_t bytes, std::uint64_t base)                                             \
    {                                                                                                                \
        char *at = static_cast<char *>(p);                                                                           \
        char *const end = at + bytes;                                                                                \
        __m256i v0 = lanes(base), v1 = lanes(base + 4), v2 = lanes(base + 8), v3 = lanes(base + 12);                 \
        __m256i v4 = lanes(base + 16), v5 = lanes(base + 20), v6 = lanes(base + 24), v7 = lanes(base + 28);          \
        const __m256i step = _mm256_set1_epi64x(32);                                                                 \
        for (; at < end; at += kRow) {                                                                               \
            store(reinterpret_cast<__m256i *>(at), v0);                                                              \
            store(reinterpret_cast<__m256i *>(at + 32), v1);                                                         \
            store(reinterpret_cast<__m256i *>(at + 64), v2);                                                         \
            store(reinterpret_cast<__m256i *>(at + 96), v3);                                                         \
            store(reinterpret_cast<__m256i *>(at + 128), v4);                                                        \
            store(reinterpret_cast<__m256i *>(at + 160), v5);                                                        \
            store(reinterpret_cast<__m256i *>(at + 192), v6);                                                        \
            store(reinterpret_cast<__m256i *>(at + 224), v7);                                                        \
            v0 = _mm256_add_epi64(v0, step);                                                                         \
            v1 = _mm256_add_epi64(v1, step);                                                                         \
            v2 = _mm256_add_epi64(v2, step);                                                                         \
            v3 = _mm256_add_epi64(v3, step);                                                                         \
            v4 = _mm256_add_epi64(v4, step);                                                                         \
            v5 = _mm256_add_epi64(v5, step);                                                                         \
            v6 = _mm256_add_epi64(v6, step);                                                                         \
            v7 = _mm256_add_epi64(v7, step);                                                                         \
        }                                                                                                            \
        fence;                                                                                                       \
    }

#define SB_BW_AVX2_COPY(name, store, fence)                                                                          \
    extern "C" void name(void *dst, const void *src, std::size_t bytes)                                              \
    {                                                                                                                \
        char *to = static_cast<char *>(dst);                                                                         \
        const char *from = static_cast<const char *>(src);                                                           \
        const char *const end = from + bytes;                                                                        \
        for (; from < end; from += kRow, to += kRow) {                                                               \
            const __m256i v0 = _mm256_load_si256(reinterpret_cast<const __m256i *>(from));                           \
            const __m256i v1 = _mm256_load_si256(reinterpret_cast<const __m256i *>(from + 32));                      \
            const __m256i v2 = _mm256_load_si256(reinterpret_cast<const __m256i *>(from + 64));                      \
            const __m256i v3 = _mm256_load_si256(reinterpret_cast<const __m256i *>(from + 96));                      \
            const __m256i v4 = _mm256_load_si256(reinterpret_cast<const __m256i *>(from + 128));                     \
            const __m256i v5 = _mm256_load_si256(reinterpret_cast<const __m256i *>(from + 160));                     \
            const __m256i v6 = _mm256_load_si256(reinterpret_cast<const __m256i *>(from + 192));                     \
            const __m256i v7 = _mm256_load_si256(reinterpret_cast<const __m256i *>(from + 224));                     \
            store(reinterpret_cast<__m256i *>(to), v0);                                                              \
            store(reinterpret_cast<__m256i *>(to + 32), v1);                                                         \
            store(reinterpret_cast<__m256i *>(to + 64), v2);                                                         \
            store(reinterpret_cast<__m256i *>(to + 96), v3);                                                         \
            store(reinterpret_cast<__m256i *>(to + 128), v4);                                                        \
            store(reinterpret_cast<__m256i *>(to + 160), v5);                                                        \
            store(reinterpret_cast<__m256i *>(to + 192), v6);                                                        \
            store(reinterpret_cast<__m256i *>(to + 224), v7);                                                        \
        }                                                                                                            \
        fence;                                                                                                       \
    }

SB_BW_AVX2_WRITE(sb_bw_avx2_write, _mm256_store_si256, (void)0)
SB_BW_AVX2_WRITE(sb_bw_avx2_write_nt, _mm256_stream_si256, _mm_sfence())
SB_BW_AVX2_COPY(sb_bw_avx2_copy, _mm256_store_si256, (void)0)
SB_BW_AVX2_COPY(sb_bw_avx2_copy_nt, _mm256_stream_si256, _mm_sfence())
