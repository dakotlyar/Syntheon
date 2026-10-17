// SPDX-License-Identifier: GPL-3.0-or-later
// The SIMD kernel's AVX-512 path (Phase 2 design §5.3, P8, P9): built with
// -mavx512f -mavx512bw -mavx512dq -mavx512vl -mfma / /arch:AVX512, and only
// ever called after the dispatch has seen AVX-512 F, BW, DQ and VL with the
// OS saving the ZMM state. THE ODR RULE (simd_common.h): this file includes
// only <immintrin.h> and simd_common.h, keeps its code in
// sb::cpu::simd_avx512, and exports only sb_simd_avx512_unit.
#include "tests/cpu/kernels/simd/simd_common.h"

#include <immintrin.h>

namespace sb::cpu::simd_avx512 {

namespace {

using simd::kDotLength;
using simd::kDotReps;
using simd::kDotShifts;
using simd::kGemmK;
using simd::kGemmN;
using simd::kGemmReps;

// C (8 x 16) = sum over r of A x B_r: one ZMM register per row of C, and two
// sets of them (even and odd k, added at the end) so sixteen FMAs are in
// flight: eight don't cover the FMA latency on every AVX-512 CPU.
void gemm(const float *a, const float *b, float *c)
{
    __m512 c0 = _mm512_setzero_ps(), c1 = _mm512_setzero_ps(), c2 = _mm512_setzero_ps(), c3 = _mm512_setzero_ps();
    __m512 c4 = _mm512_setzero_ps(), c5 = _mm512_setzero_ps(), c6 = _mm512_setzero_ps(), c7 = _mm512_setzero_ps();
    __m512 d0 = _mm512_setzero_ps(), d1 = _mm512_setzero_ps(), d2 = _mm512_setzero_ps(), d3 = _mm512_setzero_ps();
    __m512 d4 = _mm512_setzero_ps(), d5 = _mm512_setzero_ps(), d6 = _mm512_setzero_ps(), d7 = _mm512_setzero_ps();
    for (int r = 0; r < kGemmReps; ++r) {
        const float *br = b + (r % 16) * kGemmN;
        for (int k = 0; k < kGemmK; k += 2) {
            const __m512 bk = _mm512_loadu_ps(br + k * kGemmN);
            const __m512 bl = _mm512_loadu_ps(br + (k + 1) * kGemmN);
            c0 = _mm512_fmadd_ps(_mm512_set1_ps(a[0 * kGemmK + k]), bk, c0);
            c1 = _mm512_fmadd_ps(_mm512_set1_ps(a[1 * kGemmK + k]), bk, c1);
            c2 = _mm512_fmadd_ps(_mm512_set1_ps(a[2 * kGemmK + k]), bk, c2);
            c3 = _mm512_fmadd_ps(_mm512_set1_ps(a[3 * kGemmK + k]), bk, c3);
            c4 = _mm512_fmadd_ps(_mm512_set1_ps(a[4 * kGemmK + k]), bk, c4);
            c5 = _mm512_fmadd_ps(_mm512_set1_ps(a[5 * kGemmK + k]), bk, c5);
            c6 = _mm512_fmadd_ps(_mm512_set1_ps(a[6 * kGemmK + k]), bk, c6);
            c7 = _mm512_fmadd_ps(_mm512_set1_ps(a[7 * kGemmK + k]), bk, c7);
            d0 = _mm512_fmadd_ps(_mm512_set1_ps(a[0 * kGemmK + k + 1]), bl, d0);
            d1 = _mm512_fmadd_ps(_mm512_set1_ps(a[1 * kGemmK + k + 1]), bl, d1);
            d2 = _mm512_fmadd_ps(_mm512_set1_ps(a[2 * kGemmK + k + 1]), bl, d2);
            d3 = _mm512_fmadd_ps(_mm512_set1_ps(a[3 * kGemmK + k + 1]), bl, d3);
            d4 = _mm512_fmadd_ps(_mm512_set1_ps(a[4 * kGemmK + k + 1]), bl, d4);
            d5 = _mm512_fmadd_ps(_mm512_set1_ps(a[5 * kGemmK + k + 1]), bl, d5);
            d6 = _mm512_fmadd_ps(_mm512_set1_ps(a[6 * kGemmK + k + 1]), bl, d6);
            d7 = _mm512_fmadd_ps(_mm512_set1_ps(a[7 * kGemmK + k + 1]), bl, d7);
        }
    }
    _mm512_storeu_ps(c + 0 * kGemmN, _mm512_add_ps(c0, d0));
    _mm512_storeu_ps(c + 1 * kGemmN, _mm512_add_ps(c1, d1));
    _mm512_storeu_ps(c + 2 * kGemmN, _mm512_add_ps(c2, d2));
    _mm512_storeu_ps(c + 3 * kGemmN, _mm512_add_ps(c3, d3));
    _mm512_storeu_ps(c + 4 * kGemmN, _mm512_add_ps(c4, d4));
    _mm512_storeu_ps(c + 5 * kGemmN, _mm512_add_ps(c5, d5));
    _mm512_storeu_ps(c + 6 * kGemmN, _mm512_add_ps(c6, d6));
    _mm512_storeu_ps(c + 7 * kGemmN, _mm512_add_ps(c7, d7));
}

std::int64_t dot(const std::int16_t *x, const std::int16_t *y)
{
    __m512i s0 = _mm512_setzero_si512(), s1 = _mm512_setzero_si512();
    for (int i = 0; i < kDotLength; i += 64) {
        s0 = _mm512_add_epi32(s0, _mm512_madd_epi16(_mm512_loadu_si512(x + i), _mm512_loadu_si512(y + i)));
        s1 = _mm512_add_epi32(s1, _mm512_madd_epi16(_mm512_loadu_si512(x + i + 32), _mm512_loadu_si512(y + i + 32)));
    }
    std::int32_t lanes[16];
    _mm512_storeu_si512(lanes, _mm512_add_epi32(s0, s1));
    std::int64_t total = 0;
    for (const std::int32_t v : lanes)
        total += v;
    return total;
}

} // namespace

} // namespace sb::cpu::simd_avx512

extern "C" void sb_simd_avx512_unit(const sb::cpu::simd::Input *in, sb::cpu::simd::Output *out)
{
    using namespace sb::cpu::simd;
    sb::cpu::simd_avx512::gemm(in->a, in->b, out->c);
    std::int64_t sum = 0, weighted = 0;
    for (int p = 0; p < kDotReps; ++p) {
        const std::int64_t d = sb::cpu::simd_avx512::dot(in->x, in->y + p % kDotShifts);
        sum += d;
        weighted += std::int64_t(p + 1) * d;
    }
    out->dotSum = sum;
    out->dotWeighted = weighted;
    out->vectorBits = 512;
}
