// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The integer kernel (Phase 2 design §5.1): `int.rate`, and `single.int` on
// one pinned thread. One unit, for a seed s in 0..63, is three fixed parts:
//
//   1. ALU: 8 independent 64-bit chains x 2^19 steps of shift-xor, rotate,
//      multiply, add. The chains expose instruction-level parallelism; the
//      dependency inside each chain (and the 64-bit multiply, which the x86-64
//      and ARMv8 baselines can't vectorise) keeps it scalar.
//   2. Division: 4 chains x 2^15 steps (2^17 in all) of a 64-bit div and mod,
//      the divisor taken from the chain's own data. The dividend always has
//      its top bit set, so no compiler can take a 32-bit division shortcut.
//   3. Branchy: a bytecode interpreter (switch dispatch, 12 opcodes) runs a
//      fixed 256-instruction program for 2^18 dispatches; skips and jumps
//      depend on the data, so branch and indirect-branch prediction is
//      exercised.
//
// The sizes are the design's (§5.1) times 32, all three parts together, so a
// unit takes about 2 ms on Knowhere, not 0.06 ms: §4.5 needs units of at
// least 1 ms for the per-unit timing to cost under 0.01% (AP-5 M1).
//
// Counted operations (kIntOpsPerUnit): every 64-bit integer operator in parts
// 1 and 2 as written (ALU step: shift, xor, rotate, multiply, add = 5; division
// step: shift, or, or, div, mod, shift, xor, xor, multiply, add = 10), plus one
// per interpreted instruction.
//
// The checksum folds the final chain and register values; golden/int.inc
// holds it for each seed.

#include "tests/cpu/engine/kernel.h"

#include <cstdint>

namespace sb::cpu {

inline constexpr std::uint64_t kIntAluChains = 8;
inline constexpr std::uint64_t kIntAluSteps = 1u << 19;
inline constexpr std::uint64_t kIntDivChains = 4;
inline constexpr std::uint64_t kIntDivSteps = 1u << 15;
inline constexpr std::uint64_t kIntDispatches = 1u << 18;
inline constexpr std::uint64_t kIntOpsPerUnit =
    kIntAluChains * kIntAluSteps * 5 + kIntDivChains * kIntDivSteps * 10 + kIntDispatches;  // 22544384

// One unit for `seed` (0..63); returns its checksum.
std::uint64_t intUnit(std::uint64_t seed);

class IntKernel : public Kernel
{
public:
    std::string name() const override { return "Integer"; }
    double workPerUnit() const override { return double(kIntOpsPerUnit); }
    // P14: 8 integer operations per cycle per thread (§6.3).
    double maxWorkPerCyclePerThread() const override { return 8; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;
};

} // namespace sb::cpu
