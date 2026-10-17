// SPDX-License-Identifier: GPL-3.0-or-later
// The SIMD kernel's NEON path (Phase 2 design §5.3, P9): the armv8-a
// baseline every SynBench ARM64 build assumes, so no extra arch flags. Kept to
// the same shape as the wide paths (simd_common.h): <arm_neon.h> and
// simd_common.h only, code in sb::cpu::simd_neon, one entry point.
#include "tests/cpu/kernels/simd/simd_common.h"

#include <arm_neon.h>

namespace sb::cpu::simd_neon {

namespace {

using simd::kDotLength;
using simd::kDotReps;
using simd::kDotShifts;
using simd::kGemmK;
using simd::kGemmN;
using simd::kGemmReps;

// C (8 x 16) = sum over r of A x B_r, four rows at a time: four Q registers
// per row, sixteen accumulators (of the 32 vector registers).
void gemm(const float *a, const float *b, float *c)
{
    for (int half = 0; half < 2; ++half) {
        const float *ah = a + half * 4 * kGemmK;
        float32x4_t c00 = vdupq_n_f32(0), c01 = vdupq_n_f32(0), c02 = vdupq_n_f32(0), c03 = vdupq_n_f32(0);
        float32x4_t c10 = vdupq_n_f32(0), c11 = vdupq_n_f32(0), c12 = vdupq_n_f32(0), c13 = vdupq_n_f32(0);
        float32x4_t c20 = vdupq_n_f32(0), c21 = vdupq_n_f32(0), c22 = vdupq_n_f32(0), c23 = vdupq_n_f32(0);
        float32x4_t c30 = vdupq_n_f32(0), c31 = vdupq_n_f32(0), c32 = vdupq_n_f32(0), c33 = vdupq_n_f32(0);
        for (int r = 0; r < kGemmReps; ++r) {
            const float *br = b + (r % 16) * kGemmN;
            for (int k = 0; k < kGemmK; ++k) {
                const float *bk = br + k * kGemmN;
                const float32x4_t b0 = vld1q_f32(bk), b1 = vld1q_f32(bk + 4), b2 = vld1q_f32(bk + 8),
                                  b3 = vld1q_f32(bk + 12);
                const float32x4_t a0 = vdupq_n_f32(ah[0 * kGemmK + k]);
                const float32x4_t a1 = vdupq_n_f32(ah[1 * kGemmK + k]);
                const float32x4_t a2 = vdupq_n_f32(ah[2 * kGemmK + k]);
                const float32x4_t a3 = vdupq_n_f32(ah[3 * kGemmK + k]);
                c00 = vfmaq_f32(c00, a0, b0);
                c01 = vfmaq_f32(c01, a0, b1);
                c02 = vfmaq_f32(c02, a0, b2);
                c03 = vfmaq_f32(c03, a0, b3);
                c10 = vfmaq_f32(c10, a1, b0);
                c11 = vfmaq_f32(c11, a1, b1);
                c12 = vfmaq_f32(c12, a1, b2);
                c13 = vfmaq_f32(c13, a1, b3);
                c20 = vfmaq_f32(c20, a2, b0);
                c21 = vfmaq_f32(c21, a2, b1);
                c22 = vfmaq_f32(c22, a2, b2);
                c23 = vfmaq_f32(c23, a2, b3);
                c30 = vfmaq_f32(c30, a3, b0);
                c31 = vfmaq_f32(c31, a3, b1);
                c32 = vfmaq_f32(c32, a3, b2);
                c33 = vfmaq_f32(c33, a3, b3);
            }
        }
        float *ch = c + half * 4 * kGemmN;
        vst1q_f32(ch + 0 * kGemmN + 0, c00);
        vst1q_f32(ch + 0 * kGemmN + 4, c01);
        vst1q_f32(ch + 0 * kGemmN + 8, c02);
        vst1q_f32(ch + 0 * kGemmN + 12, c03);
        vst1q_f32(ch + 1 * kGemmN + 0, c10);
        vst1q_f32(ch + 1 * kGemmN + 4, c11);
        vst1q_f32(ch + 1 * kGemmN + 8, c12);
        vst1q_f32(ch + 1 * kGemmN + 12, c13);
        vst1q_f32(ch + 2 * kGemmN + 0, c20);
        vst1q_f32(ch + 2 * kGemmN + 4, c21);
        vst1q_f32(ch + 2 * kGemmN + 8, c22);
        vst1q_f32(ch + 2 * kGemmN + 12, c23);
        vst1q_f32(ch + 3 * kGemmN + 0, c30);
        vst1q_f32(ch + 3 * kGemmN + 4, c31);
        vst1q_f32(ch + 3 * kGemmN + 8, c32);
        vst1q_f32(ch + 3 * kGemmN + 12, c33);
    }
}

// Widening multiply-accumulate into int32 lanes (smlal / smlal2).
std::int64_t dot(const std::int16_t *x, const std::int16_t *y)
{
    int32x4_t s0 = vdupq_n_s32(0), s1 = vdupq_n_s32(0), s2 = vdupq_n_s32(0), s3 = vdupq_n_s32(0);
    for (int i = 0; i < kDotLength; i += 16) {
        const int16x8_t x0 = vld1q_s16(x + i), y0 = vld1q_s16(y + i);
        const int16x8_t x1 = vld1q_s16(x + i + 8), y1 = vld1q_s16(y + i + 8);
        s0 = vmlal_s16(s0, vget_low_s16(x0), vget_low_s16(y0));
        s1 = vmlal_high_s16(s1, x0, y0);
        s2 = vmlal_s16(s2, vget_low_s16(x1), vget_low_s16(y1));
        s3 = vmlal_high_s16(s3, x1, y1);
    }
    return std::int64_t(vaddvq_s32(vaddq_s32(vaddq_s32(s0, s1), vaddq_s32(s2, s3))));
}

} // namespace

} // namespace sb::cpu::simd_neon

extern "C" void sb_simd_neon_unit(const sb::cpu::simd::Input *in, sb::cpu::simd::Output *out)
{
    using namespace sb::cpu::simd;
    sb::cpu::simd_neon::gemm(in->a, in->b, out->c);
    std::int64_t sum = 0, weighted = 0;
    for (int p = 0; p < kDotReps; ++p) {
        const std::int64_t d = sb::cpu::simd_neon::dot(in->x, in->y + p % kDotShifts);
        sum += d;
        weighted += std::int64_t(p + 1) * d;
    }
    out->dotSum = sum;
    out->dotWeighted = weighted;
    out->vectorBits = 128;
}
