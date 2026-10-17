// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the SIMD kernel's per-ISA TUs share with the rest of the build (Phase
// 2 design §5.3, P8). THE ODR RULE: a TU compiled with wider arch flags than
// the baseline (simd_avx2.cpp, simd_avx512.cpp, simd_sve.cpp) includes only
// its intrinsics header and this file, which holds sizes and plain structs:
// no inline functions, no templates, nothing the linker could pick from a wide
// TU for a baseline caller. Each per-ISA TU keeps its code in namespace
// sb::cpu::simd_<isa> (helpers in an anonymous namespace inside it) and
// exports exactly one extern "C" entry point, sb_simd_<isa>_unit. The
// simd_odr CTest (packaging/checks/simd-odr.py) fails if a wide object
// defines any other global or weak symbol.
//
// One unit (§5.3), the same arithmetic on every path:
//   (a) an FP32 register-blocked micro-GEMM, C (8 x 16) += A (8 x 256) x B_r
//       (256 x 16), for r = 0..kGemmReps-1, where B_r is B starting at row
//       r mod 16 (B has 256 + 16 rows); FMA where the path has it;
//   (b) int16 x int16 -> int32 multiply-accumulate dot products,
//       dot_p = sum over i < 4096 of x[i] * y[i + p mod 64], p = 0..kDotReps-1.
// The data are small integers (A, B in -2..2; x, y in -128..127), so every
// product and partial sum is an exactly representable integer (|C| <= 2^22 <
// 2^24, |dot_p| < 2^26): FMA or multiply + add, and any summation order, give
// identical bits. One golden table (from the scalar reference) holds for all
// paths.

#include <cstdint>

namespace sb::cpu::simd {

// Namespace-scope constexpr: internal linkage, so no symbol in any object.
constexpr int kGemmM = 8;
constexpr int kGemmN = 16;
constexpr int kGemmK = 256;
constexpr int kGemmBRows = kGemmK + 16;
constexpr int kGemmReps = 4096;
constexpr int kDotLength = 4096;
constexpr int kDotShifts = 64;
constexpr int kDotReps = 8192;

// One seed's inputs (read-only, shared by every thread).
struct Input {
    const float *a;         // kGemmM x kGemmK, row-major
    const float *b;         // kGemmBRows x kGemmN, row-major
    const std::int16_t *x;  // kDotLength
    const std::int16_t *y;  // kDotLength + kDotShifts
};

// One unit's results (each thread has its own).
struct Output {
    float c[kGemmM * kGemmN];   // the accumulated GEMM, row-major
    std::int64_t dotSum;        // sum of dot_p
    std::int64_t dotWeighted;   // sum of (p + 1) * dot_p
    std::uint32_t vectorBits;   // the path's vector width (SVE: as the CPU runs it)
};

} // namespace sb::cpu::simd

// The per-ISA entry points: each one is defined only in its own TU, and only
// built for its architecture.
extern "C" {
void sb_simd_sse42_unit(const sb::cpu::simd::Input *in, sb::cpu::simd::Output *out);
void sb_simd_avx2_unit(const sb::cpu::simd::Input *in, sb::cpu::simd::Output *out);
void sb_simd_avx512_unit(const sb::cpu::simd::Input *in, sb::cpu::simd::Output *out);
void sb_simd_neon_unit(const sb::cpu::simd::Input *in, sb::cpu::simd::Output *out);
void sb_simd_sve_unit(const sb::cpu::simd::Input *in, sb::cpu::simd::Output *out);
}
