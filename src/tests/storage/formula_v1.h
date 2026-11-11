// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Storage score, formula v1 (Phase 4b design §11).
//
//   normalise   n = r / A for throughput/IOPS, n = A / r for latency (lower
//               is better: GeometricMetric::lowerIsBetter)
//   D8 (2026-11-11) — six groups, one per sub-test, so every row scores:
//               seq1m_q8t1 (0.125): seq1m_q8t1.read 1.0
//               seq1m_q1t1 (0.125): seq1m_q1t1.read 1.0
//               rnd4k_q32t1 (0.225): rnd4k_q32t1.read_iops 1.0
//               rnd4k_q1t1 (0.225): rnd4k_q1t1.read_iops 1.0
//               mixed (0.15): mixed.throughput 1.0
//               latency (0.15): latency.p50 0.5, latency.p99 0.3,
//               latency.p999 0.2 (all lowerIsBetter)
//   Storage score  round(1000 x exp(sum W.ln G)), sub-scores round(1000 x G);
//               a total only for an Ok result (a Partial keeps the sub-scores
//               of its complete groups); a failed sub-test drops only its own
//               sub-score (its siblings keep theirs — the point of D8).
//
// The anchors are PLACEHOLDERS (§11): recalibrated in task 13 so Tiamat's
// NVMe lands at 1,000, at which point formulaVersion bumps to 1. Never change
// an anchor or a weight without bumping formulaVersion.
//
// Each group's id is its sub-test id (seq1m_q8t1, seq1m_q1t1, rnd4k_q32t1,
// rnd4k_q1t1, mixed, latency), matching RAM/GPU's 1 group = 1 sub-test; the
// write metrics and the random-read MB/s are reported, not scored (D1).

#include "core/result.h"
#include "core/scoring.h"

namespace sb::storage::formula_v1 {

inline constexpr double kScale = 1000.0;

// The groups, their weights and their metrics' anchors and weights (§11).
const QVector<GeometricGroup> &groups();

// score() for the Storage test: the total (Ok results only) and the sub-scores.
ScoreBreakdown score(const TestResult &result);

} // namespace sb::storage::formula_v1