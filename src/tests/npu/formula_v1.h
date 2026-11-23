// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The NPU score, formula v1 (Phase 4c design §10).
//
//   normalise    n = r / A (higher-is-better throughput)
//   groups       three equal thirds, one per sub-test (locked):
//                  classify (1/3): classify.imgs  1.0
//                  detect   (1/3): detect.imgs    1.0
//                  llm      (1/3): llm.tps        1.0  (TTFT is reported, not scored, N7)
//   NPU score    round(1000 x exp(sum W.ln G)), sub-scores round(1000 x G);
//                a total only for an Ok result (a Partial keeps the sub-scores
//                of its complete groups); a failed sub-test drops only its own
//                sub-score.
//
// The anchors are calibrated to Nivedellir (Intel Core Ultra 7 268V / Intel
// AI Boost), pack_version "2" at formulaVersion 1, dated 2026-11-22 from the
// stable run cluster (runs 8–11; run 7's classify warm-up outlier is excluded)
// so Nivedellir's own medians normalise to 1,000 (§16.3). The three values are
// recorded in formula_v1.cpp; keep the exact digits there. formulaVersion
// bumps whenever a real anchor changes, exactly as Storage's placeholder →
// frozen discipline (4b commit 6ffcc01 kept testVersion 1 / formulaVersion 0
// until task 13).

#include "core/result.h"
#include "core/scoring.h"

namespace sb::npu::formula_v1 {

inline constexpr double kScale = 1000.0;

// The three groups, their weights and their metrics' anchors and weights (§10).
const QVector<GeometricGroup> &groups();

// score() for the NPU test: the total (Ok results only) and the sub-scores.
ScoreBreakdown score(const TestResult &result);

} // namespace sb::npu::formula_v1
