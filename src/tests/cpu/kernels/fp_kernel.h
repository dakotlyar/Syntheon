// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The floating-point kernel (Phase 2 design §5.2): `fp.rate`, and `single.fp`
// on one pinned thread. One unit, for a seed s in 0..63, is 131072 rounds (about 2 ms on Knowhere, §4.5) of:
//
//   FP64  8 independent chains, 4 steps each of the logistic map in Horner
//         form, z = (z * -0.925 + 3.7) * z (3 FLOPs), which stays in about
//         [1.03, 3.7]: no subnormal slow paths, and chaotic, so it never
//         settles into a constant;
//   FP64  one divide / square-root step, x = sqrt(x * 1.5 + z0) / 1.25
//         (4 FLOPs), fed by the first chain;
//   FP32  the same 8 x 4 logistic steps in float.
//
// Only + - x / and sqrt, all correctly rounded in IEEE 754, with no FP
// contraction (P7): the results are bit-exact on every compiler and CPU, so
// golden/fp.inc (the bits of the final values, folded) holds everywhere.
// FLOPs are counted as written; div and sqrt count 1 each.

#include "tests/cpu/engine/kernel.h"

#include <cstdint>

namespace sb::cpu {

inline constexpr std::uint64_t kFpRounds = 131072;
inline constexpr std::uint64_t kFpOpsPerUnit = kFpRounds * (8 * 4 * 3 + 4 + 8 * 4 * 3);  // 25690112

std::uint64_t fpUnit(std::uint64_t seed);

class FpKernel : public Kernel
{
public:
    std::string name() const override { return "Floating-point"; }
    double workPerUnit() const override { return double(kFpOpsPerUnit); }
    // P14: 4 scalar FP operations per cycle per thread (§6.3).
    double maxWorkPerCyclePerThread() const override { return 4; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;
};

} // namespace sb::cpu
