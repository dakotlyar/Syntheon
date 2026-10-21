// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The RAM score, formula v1 (Phase 3 design §10, D1, D2, P2).
//
//   normalise   n = r / A for bandwidth, n = A / r for latency (lower is
//               better: GeometricMetric::lowerIsBetter)
//   groups      Latency (W 0.50): DRAM 0.75, L3 0.15, L2 0.05, L1 0.05;
//               on a core without an L3 (or whose L3 is too close to its
//               L2 to measure, §5.2): DRAM 0.75, L2 0.20, L1 0.05 (D1, D2)
//               Bandwidth (W 0.35): all-thread read / write / copy 2/9
//               each, single-thread read / write / copy 1/9 each; the x64
//               plain-store metrics are never scored (D4)
//               Loaded latency (W 0.15): loaded.latency 1.0
//   RAM score   round(1000 x exp(sum W.ln G)), sub-scores round(1000 x G);
//               a total only for an Ok result (a Partial keeps the
//               sub-scores of its complete groups); a group whose part
//               isn't Ok has no sub-score, whatever metrics it has
//
// Whether the L3 counts is read from the stored cache.levels (scoresL3), so
// score() stays a pure function of the result.
//
// The anchors (calibration, task 14; Dave, 2026-10-20: Tiamat = 1,000, not
// Knowhere): the median of 5 RAM runs on Tiamat (AMD Ryzen 9 9950X3D,
// CachyOS, DDR5-6000 EXPO 2 x 16 GB 2R, 128-bit), rounded to 3 significant
// figures. Release build of 8c95648 (the app, through the screenshot tour,
// which Dave approved; no measurement hook), 2026-10-20 23:43-00:09 UTC, 2
// minutes apart; AC, powersave / EPP balance_performance, chase on CPU 9,
// 2 MiB THP pages on 100% of the buffers, background load 0.5-0.6%; Steam,
// the browser and the Nextcloud client closed. The PipeWire filter-chain
// daemon (~9% of one core: Sonar EQ, HeSuVi surround) ran during all 5,
// exempted from the 5% quiet check by Dave. Every scored metric's CV <= 0.26%,
// the RAM score's 0.04%; re-scored, the runs give 999-1,000. testVersion =
// formulaVersion = 1. Never change an anchor, a weight or the formula
// without bumping formulaVersion.

#include "core/result.h"
#include "core/scoring.h"

namespace sb::ram::formula_v1 {

inline constexpr double kScale = 1000.0;

// The groups, their weights and their metrics' anchors and weights, with or
// without the L3 (D1).
const QVector<GeometricGroup> &groups(bool withL3);

// Whether the result's core had an L3 that was measured: cache.levels names
// an "L3 " level not marked "not measured" (§5.2's range rule).
bool scoresL3(const TestResult &result);

// score() for the RAM test: the total (Ok results only) and the sub-scores.
ScoreBreakdown score(const TestResult &result);

} // namespace sb::ram::formula_v1
