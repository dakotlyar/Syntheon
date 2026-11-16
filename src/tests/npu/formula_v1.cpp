// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/formula_v1.h"

namespace sb::npu::formula_v1 {

namespace {

// Placeholder anchors (1.0) — calibrated in task 19 to Nivedellir = 1,000
// (§16.3). Never change a real anchor without bumping formulaVersion.
constexpr double kClassifyAnchor = 1.0;  // img/s
constexpr double kDetectAnchor = 1.0;    // img/s
constexpr double kLlmAnchor = 1.0;       // tok/s

} // namespace

const QVector<GeometricGroup> &groups()
{
    // Equal thirds across the three workloads (locked); TTFT is reported, not
    // scored (N7), so it is absent here.
    static const QVector<GeometricGroup> g = {
        {QStringLiteral("classify"), 1.0 / 3.0, {{QStringLiteral("classify.imgs"), kClassifyAnchor, 1.0}}},
        {QStringLiteral("detect"), 1.0 / 3.0, {{QStringLiteral("detect.imgs"), kDetectAnchor, 1.0}}},
        {QStringLiteral("llm"), 1.0 / 3.0, {{QStringLiteral("llm.tps"), kLlmAnchor, 1.0}}},
    };
    return g;
}

ScoreBreakdown score(const TestResult &result)
{
    const GeometricScore s = weightedGeometricScore(result, kScale, groups());
    ScoreBreakdown b;
    b.subScores = s.groups;
    // A sub-test that ended in Error (or N/A, Stop) drops only its own
    // sub-score (group id == sub-test id, the RAM/Storage rule).
    for (const SubResult &sub : result.subResults) {
        if (sub.status != Status::Ok)
            b.subScores.remove(sub.id);
    }
    // Only an Ok result gets a total; a Partial keeps its complete sub-scores.
    if (result.status == Status::Ok && b.subScores.size() == s.groups.size())
        b.total = s.total;
    return b;
}

} // namespace sb::npu::formula_v1
