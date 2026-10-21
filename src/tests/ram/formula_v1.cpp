// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/formula_v1.h"

namespace sb::ram::formula_v1 {

namespace {

// The anchors (see the header): the median of 5 quiet runs on Tiamat,
// 2026-10-20, 3 significant figures. Frozen: never change one without
// bumping formulaVersion.
constexpr double kL1 = 0.701;     // ns
constexpr double kL2 = 2.45;      // ns
constexpr double kL3 = 8.44;      // ns
constexpr double kDram = 75.2;    // ns
constexpr double kRead = 67.5;    // GB/s, all threads
constexpr double kWrite = 63.7;   // GB/s, all threads, non-temporal stores
constexpr double kCopy = 58.8;    // GB/s, all threads, read + written
constexpr double kStRead = 63.2;  // GB/s, single thread
constexpr double kStWrite = 31.3;
constexpr double kStCopy = 58.6;
constexpr double kLoaded = 176;   // ns, full load

GeometricGroup bandwidthGroup()
{
    const double all = 2.0 / 9.0, single = 1.0 / 9.0;
    return {QStringLiteral("bandwidth"), 0.35,
            {{QStringLiteral("bandwidth.read"), kRead, all},
             {QStringLiteral("bandwidth.write"), kWrite, all},
             {QStringLiteral("bandwidth.copy"), kCopy, all},
             {QStringLiteral("bandwidth.st.read"), kStRead, single},
             {QStringLiteral("bandwidth.st.write"), kStWrite, single},
             {QStringLiteral("bandwidth.st.copy"), kStCopy, single}}};
}

GeometricGroup loadedGroup()
{
    return {QStringLiteral("loaded"), 0.15, {{QStringLiteral("loaded.latency"), kLoaded, 1.0, true}}};
}

} // namespace

const QVector<GeometricGroup> &groups(bool withL3)
{
    static const QVector<GeometricGroup> l3 = {
        {QStringLiteral("latency"), 0.50,
         {{QStringLiteral("latency.dram"), kDram, 0.75, true},
          {QStringLiteral("latency.l3"), kL3, 0.15, true},
          {QStringLiteral("latency.l2"), kL2, 0.05, true},
          {QStringLiteral("latency.l1"), kL1, 0.05, true}}},
        bandwidthGroup(),
        loadedGroup(),
    };
    // D1, D2: no L3, its weight goes to the L2 (the last-level cache).
    static const QVector<GeometricGroup> noL3 = {
        {QStringLiteral("latency"), 0.50,
         {{QStringLiteral("latency.dram"), kDram, 0.75, true},
          {QStringLiteral("latency.l2"), kL2, 0.20, true},
          {QStringLiteral("latency.l1"), kL1, 0.05, true}}},
        bandwidthGroup(),
        loadedGroup(),
    };
    return withL3 ? l3 : noL3;
}

bool scoresL3(const TestResult &result)
{
    const Metric *m = result.metric(QStringLiteral("cache.levels"));
    if (!m || m->isNumeric())
        return false;
    // "… · L3 32 MiB (shared by CPUs 8–15, 24–31)", or "… · L3 1 MiB (too
    // close to the L2: not measured)" for one §5.2 leaves out.
    for (const QString &part : std::get<QString>(m->value).split(QStringLiteral(" · "))) {
        if (part.startsWith(QLatin1StringView("L3 ")))
            return !part.contains(QLatin1StringView("not measured"));
    }
    return false;
}

ScoreBreakdown score(const TestResult &result)
{
    const GeometricScore s = weightedGeometricScore(result, kScale, groups(scoresL3(result)));
    ScoreBreakdown b;
    b.subScores = s.groups;
    // A group whose part ended in an Error (or N/A, Stop) gets no sub-score
    // even if its scored metrics are all there (AP-5 M2: a plain-store
    // check failing after the scored bandwidth measurements).
    for (const SubResult &sub : result.subResults) {
        if (sub.status != Status::Ok)
            b.subScores.remove(sub.id);
    }
    // Only an Ok result gets a total (§10.2); a Partial keeps the sub-scores
    // of its complete parts.
    if (result.status == Status::Ok && b.subScores.size() == s.groups.size())
        b.total = s.total;
    return b;
}

} // namespace sb::ram::formula_v1
