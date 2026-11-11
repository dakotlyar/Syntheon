// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/storage/formula_v1.h"

namespace sb::storage::formula_v1 {

namespace {

// The anchors (§11). PLACEHOLDERS: recalibrated in task 13 so Tiamat's NVMe
// lands at 1,000 (the constant/anchors are the only things task 13 changes).
// Frozen only then; never change one without bumping formulaVersion.
constexpr double kSeqQ8 = 7000.0;     // MB/s, seq1m_q8t1.read
constexpr double kSeqQ1 = 3900.0;     // MB/s, seq1m_q1t1.read
constexpr double kRndQ32 = 1810000.0; // IOPS, rnd4k_q32t1.read_iops (placeholder: a real NVMe is ~19k-44k)
constexpr double kRndQ1 = 20000.0;    // IOPS, rnd4k_q1t1.read_iops
constexpr double kMixed = 1100.0;     // MB/s, mixed.throughput
constexpr double kLatP50 = 68.0;      // µs, latency.p50
constexpr double kLatP99 = 210.0;     // µs, latency.p99
constexpr double kLatP999 = 1840.0;   // µs, latency.p999

} // namespace

const QVector<GeometricGroup> &groups()
{
    // D8 (Dave, 2026-11-11): six groups, one per sub-test, so every sub-test
    // row scores (matching RAM/GPU's 1 group = 1 sub-test). The total is
    // mathematically identical to the earlier four-group reading — 0.25 x
    // (0.5·ln q8 + 0.5·ln q1) ≡ 0.125·ln q8 + 0.125·ln q1 — only the
    // per-sub-test surface changes (a failed sub-test drops only its own
    // sub-score, not a sibling's).
    static const QVector<GeometricGroup> g = {
        {QStringLiteral("seq1m_q8t1"), 0.125, {{QStringLiteral("seq1m_q8t1.read"), kSeqQ8, 1.0}}},
        {QStringLiteral("seq1m_q1t1"), 0.125, {{QStringLiteral("seq1m_q1t1.read"), kSeqQ1, 1.0}}},
        {QStringLiteral("rnd4k_q32t1"), 0.225, {{QStringLiteral("rnd4k_q32t1.read_iops"), kRndQ32, 1.0}}},
        {QStringLiteral("rnd4k_q1t1"), 0.225, {{QStringLiteral("rnd4k_q1t1.read_iops"), kRndQ1, 1.0}}},
        {QStringLiteral("mixed"), 0.15, {{QStringLiteral("mixed.throughput"), kMixed, 1.0}}},
        {QStringLiteral("latency"), 0.15,
         {{QStringLiteral("latency.p50"), kLatP50, 0.5, true},
          {QStringLiteral("latency.p99"), kLatP99, 0.3, true},
          {QStringLiteral("latency.p999"), kLatP999, 0.2, true}}},
    };
    return g;
}

ScoreBreakdown score(const TestResult &result)
{
    const GeometricScore s = weightedGeometricScore(result, kScale, groups());
    ScoreBreakdown b;
    b.subScores = s.groups;
    // A sub-test that ended in an Error (or N/A, Stop) drops only its own
    // sub-score (D8: group id == sub-test id, so a sibling keeps its score).
    for (const SubResult &sub : result.subResults) {
        if (sub.status != Status::Ok)
            b.subScores.remove(sub.id);
    }
    // Only an Ok result gets a total; a Partial keeps the sub-scores of its
    // complete parts.
    if (result.status == Status::Ok && b.subScores.size() == s.groups.size())
        b.total = s.total;
    return b;
}

} // namespace sb::storage::formula_v1
