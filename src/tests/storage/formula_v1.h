// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Storage score, formula v1 (Phase 4b design §11).
//
//   normalise   n = r / A for throughput/IOPS, n = A / r for latency (lower
//               is better: GeometricMetric::lowerIsBetter)
//   groups      Sequential (W 0.25): seq1m_q8t1.read 0.5, seq1m_q1t1.read 0.5
//               Random 4K (W 0.45): rnd4k_q32t1.read_iops 0.5,
//               rnd4k_q1t1.read_iops 0.5
//               Mixed (W 0.15): mixed.throughput 1.0
//               Latency (W 0.15): latency.p50 0.5, latency.p99 0.3,
//               latency.p999 0.2 (all lowerIsBetter)
//   Storage score  round(1000 x exp(sum W.ln G)), sub-scores round(1000 x G);
//               a total only for an Ok result (a Partial keeps the sub-scores
//               of its complete groups); a group whose part isn't Ok has no
//               sub-score, whatever metrics it has.
//
// The anchors are PLACEHOLDERS (§11): recalibrated in task 13 so Tiamat's
// NVMe lands at 1,000, at which point formulaVersion bumps to 1. Never change
// an anchor or a weight without bumping formulaVersion.
//
// The group ids are "sequential", "random", "mixed", "latency" (§11's group
// names). "mixed" and "latency" coincide with their sub-test ids; "sequential"
// and "random" each span two sub-tests (seq1m_q8t1+seq1m_q1t1 and
// rnd4k_q32t1+rnd4k_q1t1), so the per-sub-test scoring the Results page shows
// for those is reconciled when the assembled test wires its subResults (task 10).

#include "core/result.h"
#include "core/scoring.h"

namespace sb::storage::formula_v1 {

inline constexpr double kScale = 1000.0;

// The groups, their weights and their metrics' anchors and weights (§11).
const QVector<GeometricGroup> &groups();

// score() for the Storage test: the total (Ok results only) and the sub-scores.
ScoreBreakdown score(const TestResult &result);

} // namespace sb::storage::formula_v1