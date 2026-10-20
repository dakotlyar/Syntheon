// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The cache levels found on the latency curve (Phase 3 design §5.2, R5):
// the fallback when the OS reports no caches for the chased core (some
// Linux ARM boards, VMs). Pure.
//
// A knee is a grid point where the latency rises by >= 25% to the next
// point (the next half-octave) and keeps rising to the point after that, so
// a one-point spike (noise) is not a knee. The steep part after a knee
// belongs to that knee: the search resumes once a step rises by less than
// 25%. The cache sizes are the sizes at the knees (the last point of each
// plateau): plateaus between knees are L1, L2, (L3), and the plateau after
// the last knee is DRAM. So n knees give n cache levels; with fewer than two
// the levels that can't be found are absent, and the latency group can't be
// scored (§5.2: the test is then Partial).

#include "tests/ram/sweep/targets.h"

#include <cstdint>
#include <vector>

namespace sb::ram {

struct CurvePoint {
    std::uint64_t bytes = 0;
    double ns = 0;
};

inline constexpr double kKneeRise = 1.25;

// The sizes just before each knee, ascending. `curve` is ascending by size.
std::vector<std::uint64_t> detectKnees(const std::vector<CurvePoint> &curve);

// The targets from the curve's knees: the first three knees are C1..C3 (a
// fourth and later only move C_last, which doesn't change anything here),
// with targetsFromSizes' rules; fromCurve is set. `dramBytes` is the DRAM
// target the grid was measured up to (the latency buffer's size, fixed in
// prepare before any curve exists).
LatencyTargets targetsFromCurve(const std::vector<CurvePoint> &curve, std::uint64_t dramBytes);

} // namespace sb::ram
