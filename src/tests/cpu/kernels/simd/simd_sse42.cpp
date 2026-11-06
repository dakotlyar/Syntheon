// SPDX-License-Identifier: GPL-3.0-or-later
// The SIMD kernel's SSE4.2 path (Phase 2 design §5.3, P9): the x86-64-v2
// baseline every Syntheon x64 build already assumes, so no extra arch flags.
// It uses SSE/SSE2 instructions only (multiply + add, no FMA). Kept to the
// same shape as the wide paths (simd_common.h): <immintrin.h> and
// simd_common.h only, code in sb::cpu::simd_sse42, one entry point.
#include "tests/cpu/kernels/simd/simd_common.h"

#include <immintrin.h>

namespace sb::cpu::simd_sse42 {

namespace {

using simd::kDotLength;
using simd::kDotReps;
using simd::kDotShifts;
using simd::kGemmK;
using simd::kGemmN;
using simd::kGemmReps;

// C (8 x 16) = sum over r of A x B_r, two rows at a time: four XMM registers
// per row, eight accumulators.
void gemm(const float *a, const float *b, float *c)
{
    for (int pair = 0; pair < 4; ++pair) {
        const float *ap = a + pair * 2 * kGemmK;
        __m128 c00 = _mm_setzero_ps(), c01 = _mm_setzero_ps(), c02 = _mm_setzero_ps(), c03 = _mm_setzero_ps();
        __m128 c10 = _mm_setzero_ps(), c11 = _mm_setzero_ps(), c12 = _mm_setzero_ps(), c13 = _mm_setzero_ps();
        for (int r = 0; r < kGemmReps; ++r) {
            const float *br = b + (r % 16) * kGemmN;
            for (int k = 0; k < kGemmK; ++k) {
                const float *bk = br + k * kGemmN;
                const __m128 b0 = _mm_loadu_ps(bk), b1 = _mm_loadu_ps(bk + 4), b2 = _mm_loadu_ps(bk + 8),
                             b3 = _mm_loadu_ps(bk + 12);
                const __m128 a0 = _mm_set1_ps(ap[k]);
                const __m128 a1 = _mm_set1_ps(ap[kGemmK + k]);
                c00 = _mm_add_ps(c00, _mm_mul_ps(a0, b0));
                c01 = _mm_add_ps(c01, _mm_mul_ps(a0, b1));
                c02 = _mm_add_ps(c02, _mm_mul_ps(a0, b2));
                c03 = _mm_add_ps(c03, _mm_mul_ps(a0, b3));
                c10 = _mm_add_ps(c10, _mm_mul_ps(a1, b0));
                c11 = _mm_add_ps(c11, _mm_mul_ps(a1, b1));
                c12 = _mm_add_ps(c12, _mm_mul_ps(a1, b2));
                c13 = _mm_add_ps(c13, _mm_mul_ps(a1, b3));
            }
        }
        float *cp = c + pair * 2 * kGemmN;
        _mm_storeu_ps(cp + 0, c00);
        _mm_storeu_ps(cp + 4, c01);
        _mm_storeu_ps(cp + 8, c02);
        _mm_storeu_ps(cp + 12, c03);
        _mm_storeu_ps(cp + kGemmN + 0, c10);
        _mm_storeu_ps(cp + kGemmN + 4, c11);
        _mm_storeu_ps(cp + kGemmN + 8, c12);
        _mm_storeu_ps(cp + kGemmN + 12, c13);
    }
}

std::int64_t dot(const std::int16_t *x, const std::int16_t *y)
{
    __m128i s0 = _mm_setzero_si128(), s1 = _mm_setzero_si128();
    for (int i = 0; i < kDotLength; i += 16) {
        const __m128i x0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(x + i));
        const __m128i y0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(y + i));
        const __m128i x1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(x + i + 8));
        const __m128i y1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(y + i + 8));
        s0 = _mm_add_epi32(s0, _mm_madd_epi16(x0, y0));
        s1 = _mm_add_epi32(s1, _mm_madd_epi16(x1, y1));
    }
    std::int32_t lanes[4];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(lanes), _mm_add_epi32(s0, s1));
    return std::int64_t(lanes[0]) + lanes[1] + lanes[2] + lanes[3];
}

} // namespace

} // namespace sb::cpu::simd_sse42

extern "C" void sb_simd_sse42_unit(const sb::cpu::simd::Input *in, sb::cpu::simd::Output *out)
{
    using namespace sb::cpu::simd;
    sb::cpu::simd_sse42::gemm(in->a, in->b, out->c);
    std::int64_t sum = 0, weighted = 0;
    for (int p = 0; p < kDotReps; ++p) {
        const std::int64_t d = sb::cpu::simd_sse42::dot(in->x, in->y + p % kDotShifts);
        sum += d;
        weighted += std::int64_t(p + 1) * d;
    }
    out->dotSum = sum;
    out->dotWeighted = weighted;
    out->vectorBits = 128;
}
