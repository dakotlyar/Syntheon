// SPDX-License-Identifier: GPL-3.0-or-later
// The Storage score, formula v1 (Phase 4b design §11 [D8], §16): at the anchors
// everything is 1000; the group/metric weights act as elasticities; a missing
// metric means no total but keeps the other groups; a Partial keeps sub-scores
// and drops the total; a failed sub-test drops only its own sub-score (D8).
// Mirrors tst_ram_test::formula (N6).
#include "tests/storage/formula_v1.h"

#include <QTest>

#include <cmath>

using namespace sb;
using namespace sb::storage::formula_v1;

class TestStorageFormula : public QObject
{
    Q_OBJECT

private:
    // A completed result with every scored metric at its anchor.
    static TestResult atAnchors()
    {
        TestResult r;
        r.status = Status::Ok;
        for (const GeometricGroup &g : groups()) {
            for (const GeometricMetric &m : g.metrics)
                r.metrics.append({m.metricKey, g.id, QString(), m.anchor, QString(), Direction::Neutral, false});
        }
        r.subResults = {{QStringLiteral("seq1m_q8t1"), Status::Ok, {}, {}},
                        {QStringLiteral("seq1m_q1t1"), Status::Ok, {}, {}},
                        {QStringLiteral("rnd4k_q32t1"), Status::Ok, {}, {}},
                        {QStringLiteral("rnd4k_q1t1"), Status::Ok, {}, {}},
                        {QStringLiteral("latency"), Status::Ok, {}, {}},
                        {QStringLiteral("mixed"), Status::Ok, {}, {}}};
        return r;
    }

    static void setMetric(TestResult &r, const char *key, double factor)
    {
        for (Metric &m : r.metrics) {
            if (m.key == QLatin1StringView(key))
                m.value = std::get<double>(m.value) * factor;
        }
    }

private slots:
    void atAnchorsIsOneThousand()
    {
        const ScoreBreakdown b = score(atAnchors());
        QVERIFY(b.total);
        QCOMPARE(*b.total, 1000.0);
        QCOMPARE(b.subScores.size(), 6);
        for (const double s : b.subScores)
            QCOMPARE(s, 1000.0);
    }

    void weightsSumToOne()
    {
        double gw = 0;
        for (const GeometricGroup &g : groups()) {
            gw += g.weight;
            double mw = 0;
            for (const GeometricMetric &m : g.metrics) {
                mw += m.weight;
                // Only the latency group's metrics are lower-is-better.
                QCOMPARE(m.lowerIsBetter, g.id == QLatin1StringView("latency"));
            }
            QVERIFY(std::abs(mw - 1) < 1e-12);
        }
        QVERIFY(std::abs(gw - 1) < 1e-12);
        // The six groups, in declaration order, one per sub-test (§11 D8).
        QCOMPARE(groups().size(), 6);
        QCOMPARE(groups().at(0).id, QStringLiteral("seq1m_q8t1"));
        QCOMPARE(groups().at(1).id, QStringLiteral("seq1m_q1t1"));
        QCOMPARE(groups().at(2).id, QStringLiteral("rnd4k_q32t1"));
        QCOMPARE(groups().at(3).id, QStringLiteral("rnd4k_q1t1"));
        QCOMPARE(groups().at(4).id, QStringLiteral("mixed"));
        QCOMPARE(groups().at(5).id, QStringLiteral("latency"));
    }

    // The 8 scored metrics across 6 groups, their keys, anchors and weights,
    // exactly as §11 D8.
    void groupsMatchTheDesign()
    {
        const QVector<GeometricGroup> &g = groups();
        QCOMPARE(g.size(), 6);
        QCOMPARE(g.at(0).weight, 0.125);
        QCOMPARE(g.at(1).weight, 0.125);
        QCOMPARE(g.at(2).weight, 0.225);
        QCOMPARE(g.at(3).weight, 0.225);
        QCOMPARE(g.at(4).weight, 0.15);
        QCOMPARE(g.at(5).weight, 0.15);

        const auto &q8 = g.at(0).metrics;
        QCOMPARE(q8.size(), 1);
        QCOMPARE(q8[0].metricKey, QStringLiteral("seq1m_q8t1.read"));
        QCOMPARE(q8[0].anchor, 7000.0);
        QCOMPARE(q8[0].weight, 1.0);

        const auto &q1 = g.at(1).metrics;
        QCOMPARE(q1.size(), 1);
        QCOMPARE(q1[0].metricKey, QStringLiteral("seq1m_q1t1.read"));
        QCOMPARE(q1[0].anchor, 3900.0);
        QCOMPARE(q1[0].weight, 1.0);

        const auto &q32 = g.at(2).metrics;
        QCOMPARE(q32.size(), 1);
        QCOMPARE(q32[0].metricKey, QStringLiteral("rnd4k_q32t1.read_iops"));
        QCOMPARE(q32[0].anchor, 1810000.0);
        QCOMPARE(q32[0].weight, 1.0);

        const auto &rq1 = g.at(3).metrics;
        QCOMPARE(rq1.size(), 1);
        QCOMPARE(rq1[0].metricKey, QStringLiteral("rnd4k_q1t1.read_iops"));
        QCOMPARE(rq1[0].anchor, 20000.0);
        QCOMPARE(rq1[0].weight, 1.0);

        const auto &mixed = g.at(4).metrics;
        QCOMPARE(mixed.size(), 1);
        QCOMPARE(mixed[0].metricKey, QStringLiteral("mixed.throughput"));
        QCOMPARE(mixed[0].anchor, 1100.0);
        QCOMPARE(mixed[0].weight, 1.0);

        const auto &lat = g.at(5).metrics;
        QCOMPARE(lat.size(), 3);
        QCOMPARE(lat[0].metricKey, QStringLiteral("latency.p50"));
        QCOMPARE(lat[0].anchor, 68.0);
        QCOMPARE(lat[0].weight, 0.5);
        QVERIFY(lat[0].lowerIsBetter);
        QCOMPARE(lat[1].metricKey, QStringLiteral("latency.p99"));
        QCOMPARE(lat[1].anchor, 210.0);
        QCOMPARE(lat[1].weight, 0.3);
        QVERIFY(lat[1].lowerIsBetter);
        QCOMPARE(lat[2].metricKey, QStringLiteral("latency.p999"));
        QCOMPARE(lat[2].anchor, 1840.0);
        QCOMPARE(lat[2].weight, 0.2);
        QVERIFY(lat[2].lowerIsBetter);
    }

    void elasticities()
    {
        // Each scored metric is its own group (D8); doubling one adds its group
        // weight in octaves. These equal the earlier combined-group readings
        // (0.25 x 0.5 = 0.125, 0.45 x 0.5 = 0.225).

        // seq1m_q8t1.read doubled: +0.125 octaves.
        TestResult r = atAnchors();
        setMetric(r, "seq1m_q8t1.read", 2.0);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.125)));

        // seq1m_q1t1.read doubled: +0.125 octaves.
        r = atAnchors();
        setMetric(r, "seq1m_q1t1.read", 2.0);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.125)));

        // rnd4k_q32t1.read_iops doubled: +0.225 octaves (random is scored by IOPS).
        r = atAnchors();
        setMetric(r, "rnd4k_q32t1.read_iops", 2.0);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.225)));

        // rnd4k_q1t1.read_iops doubled: +0.225 octaves.
        r = atAnchors();
        setMetric(r, "rnd4k_q1t1.read_iops", 2.0);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.225)));

        // mixed.throughput doubled: +0.15 octaves.
        r = atAnchors();
        setMetric(r, "mixed.throughput", 2.0);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.15)));

        // latency.p50 halved (lower-is-better, metric weight 0.5, group 0.15):
        // +0.15 x 0.5 octaves.
        r = atAnchors();
        setMetric(r, "latency.p50", 0.5);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.15 * 0.5)));

        // latency.p99 halved: +0.15 x 0.3.
        r = atAnchors();
        setMetric(r, "latency.p99", 0.5);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.15 * 0.3)));

        // latency.p999 halved: +0.15 x 0.2.
        r = atAnchors();
        setMetric(r, "latency.p999", 0.5);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.15 * 0.2)));

        // The write metrics (and the random-read MB/s and mixed IOPS) are reported,
        // not scored (D1): they are present on a real result yet change nothing.
        r = atAnchors();
        for (const char *key : {"seq1m_q8t1.write", "seq1m_q1t1.write", "rnd4k_q32t1.read", "rnd4k_q32t1.write",
                                "rnd4k_q32t1.write_iops", "rnd4k_q1t1.read", "rnd4k_q1t1.write",
                                "rnd4k_q1t1.write_iops", "mixed.iops"})
            r.metrics.append({QString::fromLatin1(key), QString(), QString(), 1234.0, QString(), Direction::Neutral, false});
        QCOMPARE(*score(r).total, 1000.0);
    }

    void missingMetricDropsTotalButKeepsOthers()
    {
        TestResult r = atAnchors();
        r.metrics.removeIf([](const Metric &m) { return m.key == QLatin1StringView("mixed.throughput"); });
        const ScoreBreakdown b = score(r);
        QVERIFY(!b.total);
        QVERIFY(!b.subScores.contains(QStringLiteral("mixed")));
        QCOMPARE(b.subScores.value(QStringLiteral("seq1m_q8t1")), 1000.0);
        QCOMPARE(b.subScores.value(QStringLiteral("seq1m_q1t1")), 1000.0);
        QCOMPARE(b.subScores.value(QStringLiteral("rnd4k_q32t1")), 1000.0);
        QCOMPARE(b.subScores.value(QStringLiteral("rnd4k_q1t1")), 1000.0);
        QCOMPARE(b.subScores.value(QStringLiteral("latency")), 1000.0);

        // A non-positive metric (no log) also drops its group (its sub-test) —
        // and only that one.
        r = atAnchors();
        for (Metric &m : r.metrics)
            if (m.key == QLatin1StringView("rnd4k_q32t1.read_iops"))
                m.value = 0.0;
        const ScoreBreakdown b2 = score(r);
        QVERIFY(!b2.total);
        QVERIFY(!b2.subScores.contains(QStringLiteral("rnd4k_q32t1")));
        QCOMPARE(b2.subScores.value(QStringLiteral("rnd4k_q1t1")), 1000.0);
    }

    void partialKeepsSubScoresNotTotal()
    {
        TestResult r = atAnchors();
        r.status = Status::Partial;
        const ScoreBreakdown b = score(r);
        QVERIFY(!b.total);
        QCOMPARE(b.subScores.size(), 6);

        // A part that ended in Error loses its sub-score even with its metrics
        // present (the RAM rule, design §12).
        r = atAnchors();
        r.status = Status::Partial;
        for (SubResult &s : r.subResults)
            if (s.id == QLatin1StringView("latency"))
                s.status = Status::Error;
        const ScoreBreakdown bp = score(r);
        QVERIFY(!bp.total);
        QVERIFY(!bp.subScores.contains(QStringLiteral("latency")));
        QCOMPARE(bp.subScores.size(), 5);

        // D8: an Error in seq1m_q8t1 drops only seq1m_q8t1 — its sibling
        // seq1m_q1t1 keeps its sub-score (no shared "sequential" group).
        r = atAnchors();
        r.status = Status::Partial;
        for (SubResult &s : r.subResults)
            if (s.id == QLatin1StringView("seq1m_q8t1"))
                s.status = Status::Error;
        const ScoreBreakdown bs = score(r);
        QVERIFY(!bs.total);
        QVERIFY(!bs.subScores.contains(QStringLiteral("seq1m_q8t1")));
        QCOMPARE(bs.subScores.value(QStringLiteral("seq1m_q1t1")), 1000.0);
        QCOMPARE(bs.subScores.size(), 5);

        r = atAnchors();
        r.status = Status::Partial;
        for (SubResult &s : r.subResults)
            if (s.id == QLatin1StringView("rnd4k_q32t1"))
                s.status = Status::Error;
        const ScoreBreakdown br = score(r);
        QVERIFY(!br.total);
        QVERIFY(!br.subScores.contains(QStringLiteral("rnd4k_q32t1")));
        QCOMPARE(br.subScores.value(QStringLiteral("rnd4k_q1t1")), 1000.0);
        QCOMPARE(br.subScores.value(QStringLiteral("seq1m_q8t1")), 1000.0);
    }
};

QTEST_GUILESS_MAIN(TestStorageFormula)
#include "tst_storage_formula.moc"
