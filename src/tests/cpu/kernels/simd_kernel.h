// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The SIMD kernel (Phase 2 design §5.3, P8, P9, D15): `simd.rate` and
// `simd.path`. The unit is described in simd/simd_common.h: an FP32 micro-GEMM
// plus int16 dot products, on exact small-integer data, so the scalar
// reference here and every path (SSE4.2, AVX2 + FMA, AVX-512, NEON, SVE) give
// the same checksum, and golden/simd.inc (from the reference) checks them all.
//
// This file and simd_kernel.cpp are baseline code (P7 flags). The paths live
// in simd/simd_<isa>.cpp, compiled with their own arch flags, and are only
// reached through simdUnitFor() after the CPU's features said so.
//
// Counted operations: 2 per multiply-add (GEMM: 2 x 8 x 16 x 256 x 4096;
// dots: 2 x 4096 x 8192), 335,544,320 per unit.

#include "isa/cpu_features.h"
#include "tests/cpu/engine/kernel.h"
#include "tests/cpu/kernels/simd/simd_common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sb::cpu {

inline constexpr double kSimdOpsPerUnit = 2.0 * simd::kGemmM * simd::kGemmN * simd::kGemmK * simd::kGemmReps
                                          + 2.0 * simd::kDotLength * simd::kDotReps;

// Every seed's inputs, generated once and shared read-only by all threads
// (about 2.7 MB).
class SimdInputs
{
public:
    SimdInputs();
    simd::Input forSeed(std::uint64_t seed) const;

private:
    std::vector<float> m_a, m_b;
    std::vector<std::int16_t> m_x, m_y;
};

// The scalar reference unit: what every path must produce (vectorBits = 0).
void simdReferenceUnit(const simd::Input &in, simd::Output &out);
// The unit's checksum: C's bits and the dot sums (not the vector width).
std::uint64_t simdChecksum(const simd::Output &out);

using SimdUnitFn = void (*)(const simd::Input *, simd::Output *);
// The entry point of `path` in this build, or nullptr (another architecture,
// or SVE outside Linux ARM64). Calling it on a CPU without the path's
// features is a crash (SIGILL): callers check the features first.
SimdUnitFn simdUnitFor(isa::SimdPath path);

struct SimdChoice {
    isa::SimdPath path = isa::SimdPath::None;
    std::uint32_t vectorBits = 0;
    std::string label;  // for simd.path: "AVX-512 (512-bit)", "SVE (256-bit, unvalidated)"...
    std::string note;   // for the log: why this path
    bool warning = false;  // the note is a warning (SVE failed its self-check)
};
// The path the SIMD sub-test runs: the widest one the CPU and the OS support
// (P9, feature-based). SVE first runs one unit against the golden table and
// falls back to NEON if it differs (D15).
SimdChoice chooseSimdPath(const isa::CpuFeatures &features, const SimdInputs &inputs);
// simd.path's text for a path and its vector width.
std::string simdPathLabel(isa::SimdPath path, std::uint32_t vectorBits);

class SimdKernel : public Kernel
{
public:
    // Runs `path`, which the CPU must support (chooseSimdPath, or a test that
    // checked the runner's features: the internal way to force a path, §5.3).
    // `vectorBits`: the path's width (SVE: as measured by chooseSimdPath).
    SimdKernel(const SimdInputs &inputs, isa::SimdPath path, std::uint32_t vectorBits);
    std::string name() const override { return "SIMD"; }
    double workPerUnit() const override { return kSimdOpsPerUnit; }
    // P14: FP32 operations per cycle per thread at the path's width with two
    // FMA pipes (x86: SSE has separate multiply and add, so 4 x 4; AVX2 32;
    // AVX-512 64) or four (ARM: NEON 32; SVE 8 per 32-bit lane).
    double maxWorkPerCyclePerThread() const override;
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;

private:
    const SimdInputs &m_inputs;
    isa::SimdPath m_path;
    std::uint32_t m_vectorBits;
    SimdUnitFn m_unit;
};

} // namespace sb::cpu
