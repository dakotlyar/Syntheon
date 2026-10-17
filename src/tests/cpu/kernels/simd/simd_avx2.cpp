// SPDX-License-Identifier: GPL-3.0-or-later
// The SIMD kernel's AVX2 + FMA path (Phase 2 design §5.3, P8, P9): built with
// -mavx2 -mfma / /arch:AVX2, and only ever called after the dispatch has seen
// AVX2 and FMA with the OS saving the YMM state. THE ODR RULE
// (simd_common.h): this file includes only <immintrin.h> and simd_common.h,
// keeps its code in sb::cpu::simd_avx2, and exports only sb_simd_avx2_unit.
#include "tests/cpu/kernels/simd/simd_common.h"

#include <immintrin.h>

namespace sb::cpu::simd_avx2 {

namespace {

using simd::kDotLength;
using simd::kDotReps;
using simd::kDotShifts;
using simd::kGemmK;
using simd::kGemmN;
using simd::kGemmReps;

// C (8 x 16) = sum over r of A x B_r, four rows at a time: two YMM registers
// per row, eight accumulators (sixteen would not fit in the 16 YMM registers).
void gemm(const float *a, const float *b, float *c)
{
    for (int half = 0; half < 2; ++half) {
        const float *ah = a + half * 4 * kGemmK;
        __m256 c00 = _mm256_setzero_ps(), c01 = _mm256_setzero_ps(), c10 = _mm256_setzero_ps(), c11 = _mm256_setzero_ps();
        __m256 c20 = _mm256_setzero_ps(), c21 = _mm256_setzero_ps(), c30 = _mm256_setzero_ps(), c31 = _mm256_setzero_ps();
        for (int r = 0; r < kGemmReps; ++r) {
            const float *br = b + (r % 16) * kGemmN;
            for (int k = 0; k < kGemmK; ++k) {
                const __m256 b0 = _mm256_loadu_ps(br + k * kGemmN);
                const __m256 b1 = _mm256_loadu_ps(br + k * kGemmN + 8);
                const __m256 a0 = _mm256_set1_ps(ah[0 * kGemmK + k]);
                const __m256 a1 = _mm256_set1_ps(ah[1 * kGemmK + k]);
                const __m256 a2 = _mm256_set1_ps(ah[2 * kGemmK + k]);
                const __m256 a3 = _mm256_set1_ps(ah[3 * kGemmK + k]);
                c00 = _mm256_fmadd_ps(a0, b0, c00);
                c01 = _mm256_fmadd_ps(a0, b1, c01);
                c10 = _mm256_fmadd_ps(a1, b0, c10);
                c11 = _mm256_fmadd_ps(a1, b1, c11);
                c20 = _mm256_fmadd_ps(a2, b0, c20);
                c21 = _mm256_fmadd_ps(a2, b1, c21);
                c30 = _mm256_fmadd_ps(a3, b0, c30);
                c31 = _mm256_fmadd_ps(a3, b1, c31);
            }
        }
        float *ch = c + half * 4 * kGemmN;
        _mm256_storeu_ps(ch + 0 * kGemmN, c00);
        _mm256_storeu_ps(ch + 0 * kGemmN + 8, c01);
        _mm256_storeu_ps(ch + 1 * kGemmN, c10);
        _mm256_storeu_ps(ch + 1 * kGemmN + 8, c11);
        _mm256_storeu_ps(ch + 2 * kGemmN, c20);
        _mm256_storeu_ps(ch + 2 * kGemmN + 8, c21);
        _mm256_storeu_ps(ch + 3 * kGemmN, c30);
        _mm256_storeu_ps(ch + 3 * kGemmN + 8, c31);
    }
}

std::int64_t dot(const std::int16_t *x, const std::int16_t *y)
{
    __m256i s0 = _mm256_setzero_si256(), s1 = _mm256_setzero_si256();
    for (int i = 0; i < kDotLength; i += 32) {
        const __m256i x0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(x + i));
        const __m256i y0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(y + i));
        const __m256i x1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(x + i + 16));
        const __m256i y1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(y + i + 16));
        s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(x0, y0));
        s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(x1, y1));
    }
    std::int32_t lanes[8];
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(lanes), _mm256_add_epi32(s0, s1));
    std::int64_t total = 0;
    for (const std::int32_t v : lanes)
        total += v;
    return total;
}

} // namespace

} // namespace sb::cpu::simd_avx2

extern "C" void sb_simd_avx2_unit(const sb::cpu::simd::Input *in, sb::cpu::simd::Output *out)
{
    using namespace sb::cpu::simd;
    sb::cpu::simd_avx2::gemm(in->a, in->b, out->c);
    std::int64_t sum = 0, weighted = 0;
    for (int p = 0; p < kDotReps; ++p) {
        const std::int64_t d = sb::cpu::simd_avx2::dot(in->x, in->y + p % kDotShifts);
        sum += d;
        weighted += std::int64_t(p + 1) * d;
    }
    out->dotSum = sum;
    out->dotWeighted = weighted;
    out->vectorBits = 256;
}
