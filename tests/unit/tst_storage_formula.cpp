// SPDX-License-Identifier: GPL-3.0-or-later
// The Storage score, formula v1 (Phase 4b design §11, §16): at the anchors
// everything is 1000; the group/metric weights act as elasticities; a missing
// metric means no total but keeps the other groups; a Partial keeps sub-scores
// and drops the total. Mirrors tst_ram_test::formula (N6).
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
        QCOMPARE(b.subScores.size(), 4);
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
        // The four groups, in declaration order.
        QCOMPARE(groups().at(0).id, QStringLiteral("sequential"));
        QCOMPARE(groups().at(1).id, QStringLiteral("random"));
        QCOMPARE(groups().at(2).id, QStringLiteral("mixed"));
        QCOMPARE(groups().at(3).id, QStringLiteral("latency"));
    }

    void elasticities()
    {
        // seq1m_q8t1.read doubled: +0.25 x 0.5 octaves.
        TestResult r = atAnchors();
        setMetric(r, "seq1m_q8t1.read", 2.0);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.25 * 0.5)));

        // rnd4k_q32t1.read_iops doubled: +0.45 x 0.5 octaves (random is scored by IOPS).
        r = atAnchors();
        setMetric(r, "rnd4k_q32t1.read_iops", 2.0);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.45 * 0.5)));

        // mixed.throughput doubled: +0.15 octaves.
        r = atAnchors();
        setMetric(r, "mixed.throughput", 2.0);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.15)));

        // latency.p50 halved (lower-is-better, weight 0.5, group 0.15): +0.15 x 0.5.
        r = atAnchors();
        setMetric(r, "latency.p50", 0.5);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.15 * 0.5)));

        // latency.p999 halved: +0.15 x 0.2.
        r = atAnchors();
        setMetric(r, "latency.p999", 0.5);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 0.15 * 0.2)));

        // The write metrics are reported, not scored (D1): doubling them changes nothing.
        r = atAnchors();
        setMetric(r, "seq1m_q8t1.write", 2.0);
        setMetric(r, "rnd4k_q32t1.write_iops", 2.0);
        QCOMPARE(*score(r).total, 1000.0);
    }

    void missingMetricDropsTotalButKeepsOthers()
    {
        TestResult r = atAnchors();
        r.metrics.removeIf([](const Metric &m) { return m.key == QLatin1StringView("mixed.throughput"); });
        const ScoreBreakdown b = score(r);
        QVERIFY(!b.total);
        QVERIFY(!b.subScores.contains(QStringLiteral("mixed")));
        QCOMPARE(b.subScores.value(QStringLiteral("sequential")), 1000.0);
        QCOMPARE(b.subScores.value(QStringLiteral("random")), 1000.0);
        QCOMPARE(b.subScores.value(QStringLiteral("latency")), 1000.0);

        // A non-positive metric (no log) also drops its group.
        r = atAnchors();
        for (Metric &m : r.metrics)
            if (m.key == QLatin1StringView("rnd4k_q32t1.read_iops"))
                m.value = 0.0;
        const ScoreBreakdown b2 = score(r);
        QVERIFY(!b2.total);
        QVERIFY(!b2.subScores.contains(QStringLiteral("random")));
    }

    void partialKeepsSubScoresNotTotal()
    {
        TestResult r = atAnchors();
        r.status = Status::Partial;
        const ScoreBreakdown b = score(r);
        QVERIFY(!b.total);
        QCOMPARE(b.subScores.size(), 4);

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
        QCOMPARE(bp.subScores.size(), 3);

        // The combined groups map back: an Error in seq1m_q8t1 or seq1m_q1t1
        // drops "sequential", and rnd4k_q32t1 / rnd4k_q1t1 drop "random".
        r = atAnchors();
        r.status = Status::Partial;
        for (SubResult &s : r.subResults)
            if (s.id == QLatin1StringView("seq1m_q8t1"))
                s.status = Status::Error;
        const ScoreBreakdown bs = score(r);
        QVERIFY(!bs.total);
        QVERIFY(!bs.subScores.contains(QStringLiteral("sequential")));
        QCOMPARE(bs.subScores.value(QStringLiteral("random")), 1000.0);

        r = atAnchors();
        r.status = Status::Partial;
        for (SubResult &s : r.subResults)
            if (s.id == QLatin1StringView("rnd4k_q32t1"))
                s.status = Status::Error;
        const ScoreBreakdown br = score(r);
        QVERIFY(!br.total);
        QVERIFY(!br.subScores.contains(QStringLiteral("random")));
        QCOMPARE(br.subScores.value(QStringLiteral("sequential")), 1000.0);
    }
};

QTEST_GUILESS_MAIN(TestStorageFormula)
#include "tst_storage_formula.moc"