// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Where the scored latency levels are measured (Phase 3 design §5.2, P5,
// D1): at sizes from the chased core's own caches, in the flat middle of
// each plateau on a log axis.
//
//   L1     C1 / 2
//   Lk     sqrt(C(k-1) x Ck), k = 2, 3; it must lie in
//          [1.5 x C(k-1), 0.75 x Ck], otherwise the level is absent (a cache
//          too close in size to the one below it) and its weight moves down
//   DRAM   clamp(nextPow2(16 x C_last), 1 GiB, 2 GiB), C_last = the
//          last-level cache, whatever its level
// Each cache target is rounded down to a multiple of 4 KiB. A level the OS
// doesn't report is absent too (Oryon: no L3). Levels above 3 only count as
// C_last. When the OS reports no caches at all, the levels come from the
// curve instead (knees.h). Pure.

#include "isa/cache_topology.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sb::ram {

inline constexpr std::uint64_t kDramTargetMin = 1ull << 30;
inline constexpr std::uint64_t kDramTargetMax = 2ull << 30;

struct LevelTarget {
    int level = 0;                // 1, 2 or 3
    std::uint64_t cacheBytes = 0; // the level's size (0: not reported / not found)
    std::uint64_t bytes = 0;      // where it is measured; 0: absent
    std::string absentReason;     // why it is absent ("no L3 on this core", ...)
    bool present() const { return bytes != 0; }
};

struct LatencyTargets {
    LevelTarget caches[3];        // L1, L2, L3
    std::uint64_t dramBytes = 0;
    bool fromCurve = false;       // the sizes were found on the curve (knees.h)

    const LevelTarget &level(int n) const { return caches[n - 1]; }
};

// The DRAM target for a last-level cache of `lastLevelBytes` (0: none
// known; the minimum, 1 GiB).
std::uint64_t dramTarget(std::uint64_t lastLevelBytes);

// The targets for cache sizes C1, C2, C3 (0: that level isn't there; a
// vector shorter than 3 means the same for the missing ones) and the
// DRAM target. `fromCurve` words the absent reasons ("on this core" for
// the OS's topology, "on the curve" for the fallback) and is recorded.
LatencyTargets targetsFromSizes(const std::vector<std::uint64_t> &sizes, std::uint64_t dramBytes, bool fromCurve);

// The OS-reported topology of the chased core. Empty topology: every level
// absent ("the OS reports no caches"), DRAM 1 GiB; the caller then runs
// the curve fallback.
LatencyTargets targetsFromTopology(const isa::CacheTopology &topology);

} // namespace sb::ram
