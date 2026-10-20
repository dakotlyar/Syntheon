// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pointer chase's cycle (Phase 3 design §5.1, P3, D3): a buffer of S
// bytes is cut into lines of L bytes (the chased core's L1 line size, at
// least 64), each holding one pointer at offset 0, and all S / L lines are
// linked into a single random cycle:
//   1. an index array 0 .. n-1 (uint32_t: 2 GiB / 64 B = 32 Mi lines fits);
//   2. shuffled with Sattolo's algorithm, from xoshiro256** seeded with a
//      fixed constant per size (chaseSeed), so every run, OS and compiler
//      chases the same cycle;
//   3. line[perm[i]] = &line[perm[(i + 1) mod n]].
// Step 3 links the lines in the array's order, so the result is one cycle
// through every line whatever the permutation; Sattolo is the shuffle the
// design names, and the tests walk every built cycle in full.
//
// The build runs on the chasing thread's core, before the warm-up, and
// checks `cancelled` every 2^18 lines in each step, so Stop is honoured
// within 250 ms on a first-touched buffer (the latency buffer is touched in
// prepare, §3). §5.1 says every 1 M lines; a quarter of that keeps the
// 250 ms with room on a slow machine with 4 KiB pages, where each of the
// random writes of step 3 can cost a page walk. The index array (at most
// 128 MiB) is freed on return.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace sb::ram {

// Lines between two cancel checks (see above).
inline constexpr std::size_t kCycleCancelEvery = std::size_t(1) << 18;

// The fixed seed for a cycle of `bytes` (P3: a constant per size).
std::uint64_t chaseSeed(std::size_t bytes);

// Steps 1 and 2: the shuffled index array for `lines` lines (at least 2,
// at most 2^32). Nullopt if cancelled. Throws std::bad_alloc if the array
// can't be allocated.
std::optional<std::vector<std::uint32_t>> chaseOrder(std::size_t lines, std::uint64_t seed,
                                                     const std::function<bool()> &cancelled);

enum class CycleBuild { Ok, Cancelled, NoMemory };

// Steps 1-3 in [base, base + bytes): bytes a multiple of lineSize, lineSize
// a multiple of sizeof(void *), at least two lines. Every line's first
// pointer is written; the rest of each line is left as it was.
CycleBuild buildChaseCycle(std::byte *base, std::size_t bytes, std::size_t lineSize, std::uint64_t seed,
                           const std::function<bool()> &cancelled);

} // namespace sb::ram
