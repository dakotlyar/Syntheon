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

GeometricGroup seqGroup()
{
    // The group id is the design's name (§11), not a sub-test id: "sequential"
    // spans seq1m_q8t1 and seq1m_q1t1. How the per-sub-test scores are mapped
    // back from these combined groups is finalised with the assembled test
    // (task 10); "mixed" and "latency" coincide with their sub-test ids.
    return {QStringLiteral("sequential"), 0.25,
            {{QStringLiteral("seq1m_q8t1.read"), kSeqQ8, 0.5},
             {QStringLiteral("seq1m_q1t1.read"), kSeqQ1, 0.5}}};
}

GeometricGroup rndGroup()
{
    return {QStringLiteral("random"), 0.45,
            {{QStringLiteral("rnd4k_q32t1.read_iops"), kRndQ32, 0.5},
             {QStringLiteral("rnd4k_q1t1.read_iops"), kRndQ1, 0.5}}};
}

GeometricGroup mixedGroup()
{
    return {QStringLiteral("mixed"), 0.15, {{QStringLiteral("mixed.throughput"), kMixed, 1.0}}};
}

GeometricGroup latencyGroup()
{
    return {QStringLiteral("latency"), 0.15,
            {{QStringLiteral("latency.p50"), kLatP50, 0.5, true},
             {QStringLiteral("latency.p99"), kLatP99, 0.3, true},
             {QStringLiteral("latency.p999"), kLatP999, 0.2, true}}};
}

} // namespace

const QVector<GeometricGroup> &groups()
{
    static const QVector<GeometricGroup> g = {
        seqGroup(),
        rndGroup(),
        mixedGroup(),
        latencyGroup(),
    };
    return g;
}

// The scoring group a sub-test's result belongs to (§11): "sequential" spans
// seq1m_q8t1 and seq1m_q1t1, "random" spans rnd4k_q32t1 and rnd4k_q1t1, and
// "mixed" / "latency" coincide with their sub-test ids.
QString groupForSubTest(const QString &subTestId)
{
    if (subTestId == QLatin1StringView("seq1m_q8t1") || subTestId == QLatin1StringView("seq1m_q1t1"))
        return QStringLiteral("sequential");
    if (subTestId == QLatin1StringView("rnd4k_q32t1") || subTestId == QLatin1StringView("rnd4k_q1t1"))
        return QStringLiteral("random");
    if (subTestId == QLatin1StringView("mixed"))
        return QStringLiteral("mixed");
    if (subTestId == QLatin1StringView("latency"))
        return QStringLiteral("latency");
    return {};
}

ScoreBreakdown score(const TestResult &result)
{
    const GeometricScore s = weightedGeometricScore(result, kScale, groups());
    ScoreBreakdown b;
    b.subScores = s.groups;
    // A group whose part ended in an Error (or N/A, Stop) gets no sub-score
    // even if its scored metrics are all there (the RAM rule, design §12).
    // subScores is keyed by the group id; map the sub-test id to its group.
    for (const SubResult &sub : result.subResults) {
        if (sub.status != Status::Ok)
            b.subScores.remove(groupForSubTest(sub.id));
    }
    // Only an Ok result gets a total; a Partial keeps the sub-scores of its
    // complete parts.
    if (result.status == Status::Ok && b.subScores.size() == s.groups.size())
        b.total = s.total;
    return b;
}

} // namespace sb::storage::formula_v1