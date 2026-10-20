// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The latency sweep's grid (Phase 3 design §5.2, P4): 2^k and 1.5 x 2^k
// bytes from 4 KiB up to the DRAM target, ascending (37 points to 1 GiB, 39
// to 2 GiB). Only for the chart: each point gets short windows; the scored
// levels are measured afterwards at their own targets (targets.h). Pure.

#include <chrono>
#include <cstdint>
#include <vector>

namespace sb::ram {

inline constexpr std::uint64_t kGridFirst = 4096;

// Every grid size <= maxBytes (none below 4 KiB).
std::vector<std::uint64_t> latencyGrid(std::uint64_t maxBytes);

// §3, §5.1: the warm-up of a chase over `bytes` (lines of lineSize) at
// about `estimateNs` per hop: one full lap of the cycle, clamped to
// [lo, hi] (grid points 10 ms .. 300 ms, targets 100 ms .. 1 s).
std::chrono::milliseconds chaseWarmup(std::uint64_t bytes, std::uint64_t lineSize, double estimateNs,
                                      std::chrono::milliseconds lo, std::chrono::milliseconds hi);

} // namespace sb::ram
