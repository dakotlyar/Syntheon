// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU score, formula v1 (Phase 4c design §10, §14.1, §15): at the anchors
// everything is 1000; the three equal-third groups act as elasticities
// (2^(1/3)); a missing metric drops its group and the total; llm.ttft is
// reported, not scored; a Partial keeps sub-scores and drops the total.
#include "tests/npu/formula_v1.h"

#include <QTest>

#include <cmath>

using namespace sb;
using namespace sb::npu::formula_v1;

class TestNpuFormula : public QObject
{
    Q_OBJECT

private:
    static TestResult atAnchors()
    {
        TestResult r;
        r.status = Status::Ok;
        for (const GeometricGroup &g : groups())
            for (const GeometricMetric &m : g.metrics)
                r.metrics.append({m.metricKey, g.id, QString(), m.anchor, QString(), Direction::Neutral, false});
        r.subResults = {{QStringLiteral("classify"), Status::Ok, {}, {}},
                        {QStringLiteral("detect"), Status::Ok, {}, {}},
                        {QStringLiteral("llm"), Status::Ok, {}, {}}};
        return r;
    }

    static void setMetric(TestResult &r, const char *key, double factor)
    {
        for (Metric &m : r.metrics)
            if (m.key == QLatin1StringView(key))
                m.value = std::get<double>(m.value) * factor;
    }

private slots:
    void atAnchorsIsOneThousand()
    {
        const ScoreBreakdown b = score(atAnchors());
        QVERIFY(b.total);
        QCOMPARE(*b.total, 1000.0);
        QCOMPARE(b.subScores.size(), 3);
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
                QVERIFY(!m.lowerIsBetter);   // all three scored metrics are higher-is-better
            }
            QVERIFY(std::abs(mw - 1) < 1e-12);
        }
        QVERIFY(std::abs(gw - 1) < 1e-12);
        QCOMPARE(groups().size(), 3);
        QCOMPARE(groups().at(0).id, QStringLiteral("classify"));
        QCOMPARE(groups().at(1).id, QStringLiteral("detect"));
        QCOMPARE(groups().at(2).id, QStringLiteral("llm"));
    }

    void groupsMatchTheDesign()
    {
        const QVector<GeometricGroup> &g = groups();
        for (const GeometricGroup &gr : g)
            QCOMPARE(gr.weight, 1.0 / 3.0);
        QCOMPARE(g.at(0).metrics[0].metricKey, QStringLiteral("classify.imgs"));
        QCOMPARE(g.at(0).metrics[0].weight, 1.0);
        QCOMPARE(g.at(1).metrics[0].metricKey, QStringLiteral("detect.imgs"));
        QCOMPARE(g.at(2).metrics[0].metricKey, QStringLiteral("llm.tps"));
        // The task-19 calibration anchors, frozen in formula_v1.cpp (the exact
        // digits, so a revert to a placeholder like 1.0 fails the test — the
        // same discipline as tst_storage_formula.cpp).
        QCOMPARE(g.at(0).metrics[0].anchor, 824.150456);
        QCOMPARE(g.at(1).metrics[0].anchor, 445.047930);
        QCOMPARE(g.at(2).metrics[0].anchor, 50.229020);
        QCOMPARE(g.at(1).metrics[0].weight, 1.0);
        QCOMPARE(g.at(2).metrics[0].weight, 1.0);
    }

    void elasticities()
    {
        // Doubling any one workload adds its equal-third weight in octaves.
        TestResult r = atAnchors();
        setMetric(r, "classify.imgs", 2.0);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 1.0 / 3.0)));

        r = atAnchors();
        setMetric(r, "detect.imgs", 2.0);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 1.0 / 3.0)));

        r = atAnchors();
        setMetric(r, "llm.tps", 2.0);
        QCOMPARE(*score(r).total, std::round(1000 * std::pow(2.0, 1.0 / 3.0)));

        // llm.ttft is reported, not scored (N7): changing it changes nothing.
        r = atAnchors();
        r.metrics.append({QStringLiteral("llm.ttft"), QStringLiteral("llm"), QStringLiteral("Time to first token"), 40.0,
                          QStringLiteral("ms"), Direction::LowerIsBetter, false});
        QCOMPARE(*score(r).total, 1000.0);
        for (Metric &m : r.metrics)
            if (m.key == QLatin1StringView("llm.ttft"))
                m.value = 20.0;   // halved
        QCOMPARE(*score(r).total, 1000.0);
    }

    void missingMetricDropsTotalButKeepsOthers()
    {
        TestResult r = atAnchors();
        r.metrics.removeIf([](const Metric &m) { return m.key == QLatin1StringView("detect.imgs"); });
        const ScoreBreakdown b = score(r);
        QVERIFY(!b.total);
        QVERIFY(!b.subScores.contains(QStringLiteral("detect")));
        QCOMPARE(b.subScores.value(QStringLiteral("classify")), 1000.0);
        QCOMPARE(b.subScores.value(QStringLiteral("llm")), 1000.0);
    }

    void partialKeepsSubScoresNotTotal()
    {
        TestResult r = atAnchors();
        r.status = Status::Partial;
        const ScoreBreakdown b = score(r);
        QVERIFY(!b.total);
        QCOMPARE(b.subScores.size(), 3);

        // A failed sub-test drops only its own sub-score.
        r = atAnchors();
        r.status = Status::Partial;
        for (SubResult &s : r.subResults)
            if (s.id == QLatin1StringView("detect"))
                s.status = Status::Error;
        const ScoreBreakdown bp = score(r);
        QVERIFY(!bp.total);
        QVERIFY(!bp.subScores.contains(QStringLiteral("detect")));
        QCOMPARE(bp.subScores.value(QStringLiteral("classify")), 1000.0);
        QCOMPARE(bp.subScores.value(QStringLiteral("llm")), 1000.0);
        QCOMPARE(bp.subScores.size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestNpuFormula)
#include "tst_npu_formula.moc"
