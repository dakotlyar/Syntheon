// SPDX-License-Identifier: GPL-3.0-or-later
// The SIMD kernel's SVE path (Phase 2 design §5.3, P8, P9, D15): Linux ARM64
// only (MSVC has no SVE intrinsics), built with -march=armv8.2-a+sve, and
// only ever called after the dispatch has seen HWCAP_SVE. No machine
// available to the project runs SVE, so the dispatch runs one unit of this
// path against the golden table first and falls back to NEON if it differs
// (D15), and the path is labelled "unvalidated". Vector-length agnostic: the
// 16 columns of C are covered in chunks of svcntw() lanes under a predicate,
// so it is correct for every SVE vector length from 128 to 2048 bits.
// THE ODR RULE (simd_common.h): this file includes only <arm_sve.h> and
// simd_common.h, keeps its code in sb::cpu::simd_sve, and exports only
// sb_simd_sve_unit.
#include "tests/cpu/kernels/simd/simd_common.h"

#include <arm_sve.h>

namespace sb::cpu::simd_sve {

namespace {

using simd::kDotLength;
using simd::kDotReps;
using simd::kDotShifts;
using simd::kGemmK;
using simd::kGemmN;
using simd::kGemmReps;

// C (8 x 16) = sum over r of A x B_r: for each chunk of columns, one vector
// per row of C, in two sets (even and odd k, added at the end) so sixteen
// FMAs are in flight (SVE vectors are sizeless: the sixteen are named locals).
void gemm(const float *a, const float *b, float *c)
{
    const int lanes = int(svcntw());
    for (int j0 = 0; j0 < kGemmN; j0 += lanes) {
        const svbool_t pg = svwhilelt_b32_s32(j0, kGemmN);
        svfloat32_t c0 = svdup_n_f32(0), c1 = svdup_n_f32(0), c2 = svdup_n_f32(0), c3 = svdup_n_f32(0);
        svfloat32_t c4 = svdup_n_f32(0), c5 = svdup_n_f32(0), c6 = svdup_n_f32(0), c7 = svdup_n_f32(0);
        svfloat32_t d0 = svdup_n_f32(0), d1 = svdup_n_f32(0), d2 = svdup_n_f32(0), d3 = svdup_n_f32(0);
        svfloat32_t d4 = svdup_n_f32(0), d5 = svdup_n_f32(0), d6 = svdup_n_f32(0), d7 = svdup_n_f32(0);
        for (int r = 0; r < kGemmReps; ++r) {
            const float *br = b + (r % 16) * kGemmN + j0;
            for (int k = 0; k < kGemmK; k += 2) {
                const svfloat32_t bk = svld1_f32(pg, br + k * kGemmN);
                const svfloat32_t bl = svld1_f32(pg, br + (k + 1) * kGemmN);
                c0 = svmla_n_f32_m(pg, c0, bk, a[0 * kGemmK + k]);
                c1 = svmla_n_f32_m(pg, c1, bk, a[1 * kGemmK + k]);
                c2 = svmla_n_f32_m(pg, c2, bk, a[2 * kGemmK + k]);
                c3 = svmla_n_f32_m(pg, c3, bk, a[3 * kGemmK + k]);
                c4 = svmla_n_f32_m(pg, c4, bk, a[4 * kGemmK + k]);
                c5 = svmla_n_f32_m(pg, c5, bk, a[5 * kGemmK + k]);
                c6 = svmla_n_f32_m(pg, c6, bk, a[6 * kGemmK + k]);
                c7 = svmla_n_f32_m(pg, c7, bk, a[7 * kGemmK + k]);
                d0 = svmla_n_f32_m(pg, d0, bl, a[0 * kGemmK + k + 1]);
                d1 = svmla_n_f32_m(pg, d1, bl, a[1 * kGemmK + k + 1]);
                d2 = svmla_n_f32_m(pg, d2, bl, a[2 * kGemmK + k + 1]);
                d3 = svmla_n_f32_m(pg, d3, bl, a[3 * kGemmK + k + 1]);
                d4 = svmla_n_f32_m(pg, d4, bl, a[4 * kGemmK + k + 1]);
                d5 = svmla_n_f32_m(pg, d5, bl, a[5 * kGemmK + k + 1]);
                d6 = svmla_n_f32_m(pg, d6, bl, a[6 * kGemmK + k + 1]);
                d7 = svmla_n_f32_m(pg, d7, bl, a[7 * kGemmK + k + 1]);
            }
        }
        svst1_f32(pg, c + 0 * kGemmN + j0, svadd_f32_m(pg, c0, d0));
        svst1_f32(pg, c + 1 * kGemmN + j0, svadd_f32_m(pg, c1, d1));
        svst1_f32(pg, c + 2 * kGemmN + j0, svadd_f32_m(pg, c2, d2));
        svst1_f32(pg, c + 3 * kGemmN + j0, svadd_f32_m(pg, c3, d3));
        svst1_f32(pg, c + 4 * kGemmN + j0, svadd_f32_m(pg, c4, d4));
        svst1_f32(pg, c + 5 * kGemmN + j0, svadd_f32_m(pg, c5, d5));
        svst1_f32(pg, c + 6 * kGemmN + j0, svadd_f32_m(pg, c6, d6));
        svst1_f32(pg, c + 7 * kGemmN + j0, svadd_f32_m(pg, c7, d7));
    }
}

// SDOT (16-bit): four products summed into each 64-bit lane. Inactive lanes
// of the last chunk load as zero.
std::int64_t dot(const std::int16_t *x, const std::int16_t *y)
{
    svint64_t s = svdup_n_s64(0);
    const int step = int(svcnth());
    for (int i = 0; i < kDotLength; i += step) {
        const svbool_t pg = svwhilelt_b16_s32(i, kDotLength);
        s = svdot_s64(s, svld1_s16(pg, x + i), svld1_s16(pg, y + i));
    }
    return svaddv_s64(svptrue_b64(), s);
}

} // namespace

} // namespace sb::cpu::simd_sve

extern "C" void sb_simd_sve_unit(const sb::cpu::simd::Input *in, sb::cpu::simd::Output *out)
{
    using namespace sb::cpu::simd;
    sb::cpu::simd_sve::gemm(in->a, in->b, out->c);
    std::int64_t sum = 0, weighted = 0;
    for (int p = 0; p < kDotReps; ++p) {
        const std::int64_t d = sb::cpu::simd_sve::dot(in->x, in->y + p % kDotShifts);
        sum += d;
        weighted += std::int64_t(p + 1) * d;
    }
    out->dotSum = sum;
    out->dotWeighted = weighted;
    out->vectorBits = std::uint32_t(svcntb() * 8);
}
