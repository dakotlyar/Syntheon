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
// THE ANCHORS ARE PROVISIONAL (task 10): one real run on Tiamat (AMD Ryzen 9
// 9950X3D, CachyOS, DDR5-6000 EXPO 2 x 16 GB 2R, 2 MiB THP pages, AC, the
// desktop in use: Firefox, Steam, Caprine open; background load 2.5%) on
// 2026-10-20 at 4dd84dc (Release), rounded to 3 significant figures, so
// Tiamat scores about 1,000 for now. testVersion and formulaVersion stay 0:
// calibration (task 14) replaces them with the median of 5 quiet runs on
// Knowhere (Knowhere = 1,000) and bumps both to 1. Never change an anchor, a
// weight or the formula after that without bumping formulaVersion.

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
