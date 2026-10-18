// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The CPU score, formula v1 (Phase 2 design §7, D1, D2).
//
//   normalise   n = raw / anchor, per scored metric
//   group       G = exp(sum v.ln n), intra-group weights v (D2): FP rate 0.8 /
//               primes 0.2; the 8 compression metrics equal; AES 0.5 / SHA
//               0.5; u64 0.5 / strings 0.5; one metric elsewhere
//   CPU score   round(1000 x exp(sum W.ln G)), W = integer 0.25, FP 0.25,
//               SIMD 0.15, compression 0.10, encryption 0.10, sorting 0.10,
//               physics 0.05; nothing without every group
//   sub-scores  round(1000 x G)
//   ST mark     round(1000 x exp(5/12 ln(int/A) + 5/12 ln(fp/A) + 2/12
//               ln(compress/A))), its own three anchors (§7.4)
//
// PROVISIONAL ANCHORS: one full run on Knowhere (Linux, AC, balanced, Steam
// closed) on 2026-10-18 at 9a964e4, rounded to 3 significant figures, so
// Knowhere scores about 1,000 while the test is built. Task 18 replaces them
// with the median of 5 calibration runs and then freezes them; until then
// testVersion = formulaVersion = 0, so these scores are marked "scoring
// changed" against real ones. Anchors never change without a formulaVersion
// bump once frozen.

#include "core/result.h"
#include "core/scoring.h"

#include <optional>

namespace sb::cpu::formula_v1 {

inline constexpr double kScale = 1000.0;

// The groups, their weights and their metrics' anchors and weights.
const QVector<GeometricGroup> &groups();

// score() for the CPU test: the total and the sub-scores.
ScoreBreakdown score(const TestResult &result);

// single.mark from its three raw metrics (MOps/s, MFLOPS, MB/s); nullopt if
// one isn't positive.
std::optional<double> singleThreadMark(double singleInt, double singleFp, double singleCompress);

// The single-thread anchors (also used by the tests).
inline constexpr double kAnchorSingleInt = 12300;      // MOps/s
inline constexpr double kAnchorSingleFp = 13100;       // MFLOPS
inline constexpr double kAnchorSingleCompress = 291;   // MB/s

} // namespace sb::cpu::formula_v1
