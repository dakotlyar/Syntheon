// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU score, formula v1 (Phase 4a design §11.1, task 32). Every expected value is a property of the
// weights alone: the metrics are built from the formula's own anchors (formula_v1::groups()), so these
// tests hold for any anchor values.
#include "tests/gpu/formula_v1.h"

#include <QTest>

#include <cmath>

using namespace sb;
namespace f = sb::gpu::formula_v1;

namespace {

// An Ok result with every scored metric at `factor` x its anchor, the three sub-results Ok.
TestResult resultAt(double factor = 1.0, Status status = Status::Ok)
{
    TestResult r;
    r.testId = QStringLiteral("gpu");
    r.status = status;
    for (const GeometricGroup &g : f::groups()) {
        r.subResults.append({g.id, Status::Ok, {}, std::nullopt});
        for (const GeometricMetric &m : g.metrics) {
            Metric metric;
            metric.key = m.metricKey;
            metric.subTestId = g.id;
            metric.value = m.anchor * factor;
            r.metrics.append(metric);
        }
    }
    return r;
}

void scale(TestResult &r, const QString &key, double factor)
{
    for (Metric &m : r.metrics) {
        if (m.key == key)
            m.value = std::get<double>(m.value) * factor;
    }
}

void remove(TestResult &r, const QString &key)
{
    for (qsizetype i = 0; i < r.metrics.size(); ++i) {
        if (r.metrics[i].key == key) {
            r.metrics.removeAt(i);
            return;
        }
    }
}

} // namespace

class TestGpuFormula : public QObject
{
    Q_OBJECT

private slots:
    void shape()
    {
        const auto &g = f::groups();
        QCOMPARE(g.size(), 3);
        QCOMPARE(g[0].id, QStringLiteral("scenes3d"));
        QCOMPARE(g[1].id, QStringLiteral("native2d"));
        QCOMPARE(g[2].id, QStringLiteral("fill2d"));
        QCOMPARE(g[0].weight, 0.80);
        QCOMPARE(g[1].weight, 0.10);
        QCOMPARE(g[2].weight, 0.10);
        const QVector<double> w3d{0.20, 0.20, 0.30, 0.30};
        for (qsizetype i = 0; i < 4; ++i)
            QCOMPARE(g[0].metrics[i].weight, w3d[i]);
        QCOMPARE(g[0].metrics[0].metricKey, QStringLiteral("scenes3d.geometry.fps"));
        QCOMPARE(g[0].metrics[3].metricKey, QStringLiteral("scenes3d.particles.fps"));
        QCOMPARE(g[2].metrics.size(), 2);
        QCOMPARE(g[2].metrics[0].metricKey, QStringLiteral("fill2d.quads"));
        QCOMPARE(g[2].metrics[1].metricKey, QStringLiteral("fill2d.sprites"));
        QCOMPARE(g[2].metrics[0].weight, 0.50);
        QCOMPARE(g[2].metrics[1].weight, 0.50);
        for (const GeometricGroup &group : g) {
            for (const GeometricMetric &m : group.metrics)
                QVERIFY2(m.anchor > 0, qPrintable(m.metricKey));
        }
    }

    void allAtTheirAnchors()
    {
        const ScoreBreakdown b = f::score(resultAt());
        QCOMPARE(b.total.value_or(-1), 1000.0);
        QCOMPARE(b.subScores.size(), 3);
        QCOMPARE(b.subScores.value(QStringLiteral("scenes3d")), 1000.0);
        QCOMPARE(b.subScores.value(QStringLiteral("native2d")), 1000.0);
        QCOMPARE(b.subScores.value(QStringLiteral("fill2d")), 1000.0);
        QCOMPARE(f::score2d(resultAt()).value_or(-1), 1000.0);
    }

    // Elasticities (§11.1): Geometry 2^0.16, Shading 2^0.24, native 2D as a whole 2^0.1.
    void geometryTwiceIs1117()
    {
        TestResult r = resultAt();
        scale(r, QStringLiteral("scenes3d.geometry.fps"), 2.0);
        const ScoreBreakdown b = f::score(r);
        QCOMPARE(b.total.value_or(-1), 1117.0);
        QCOMPARE(b.subScores.value(QStringLiteral("scenes3d")), std::round(1000.0 * std::pow(2.0, 0.2)));  // within its group: weight 0.20
        QCOMPARE(b.subScores.value(QStringLiteral("native2d")), 1000.0);
        QCOMPARE(b.subScores.value(QStringLiteral("fill2d")), 1000.0);
    }

    void shadingTwiceIs1181()
    {
        TestResult r = resultAt();
        scale(r, QStringLiteral("scenes3d.shading.fps"), 2.0);
        QCOMPARE(f::score(r).total.value_or(-1), 1181.0);
    }

    void everyNativeMetricTwiceIs1072()
    {
        TestResult r = resultAt();
        for (const char *w : {"shapes", "text", "blits", "gradients"})
            scale(r, QStringLiteral("native2d.%1").arg(QLatin1String(w)), 2.0);
        const ScoreBreakdown b = f::score(r);
        QCOMPARE(b.total.value_or(-1), 1072.0);
        QCOMPARE(b.subScores.value(QStringLiteral("native2d")), 2000.0);
        QCOMPARE(b.subScores.value(QStringLiteral("scenes3d")), 1000.0);
    }

    void fillTwiceIs1072AndScore2dIsTheRoot()
    {
        TestResult r = resultAt();
        scale(r, QStringLiteral("fill2d.quads"), 2.0);
        scale(r, QStringLiteral("fill2d.sprites"), 2.0);
        QCOMPARE(f::score(r).total.value_or(-1), 1072.0);
        QCOMPARE(f::score2d(r).value_or(-1), std::round(1000.0 * std::sqrt(2.0)));  // native 1, fill 2
        // Native 4x and fill 1x: sqrt(4) = 2.
        TestResult n = resultAt();
        for (const char *w : {"shapes", "text", "blits", "gradients"})
            scale(n, QStringLiteral("native2d.%1").arg(QLatin1String(w)), 4.0);
        QCOMPARE(f::score2d(n).value_or(-1), 2000.0);
    }

    void missingSpritesHasNoTotal()
    {
        TestResult r = resultAt();
        remove(r, QStringLiteral("fill2d.sprites"));
        const ScoreBreakdown b = f::score(r);
        QVERIFY(!b.total);
        QCOMPARE(b.subScores.size(), 2);
        QVERIFY(b.subScores.contains(QStringLiteral("scenes3d")));
        QVERIFY(b.subScores.contains(QStringLiteral("native2d")));
        QVERIFY(!b.subScores.contains(QStringLiteral("fill2d")));
        QVERIFY(!f::score2d(r));
    }

    void partialHasSubScoresButNoTotal()
    {
        const ScoreBreakdown b = f::score(resultAt(1.0, Status::Partial));
        QVERIFY(!b.total);
        QCOMPARE(b.subScores.size(), 3);
        QCOMPARE(b.subScores.value(QStringLiteral("scenes3d")), 1000.0);
    }

    void aGroupThatIsNotOkHasNoSubScore()
    {
        TestResult r = resultAt(1.0, Status::Partial);
        for (SubResult &s : r.subResults) {
            if (s.id == QLatin1StringView("scenes3d"))
                s.status = Status::Error;
        }
        const ScoreBreakdown b = f::score(r);
        QVERIFY(!b.total);
        QVERIFY(!b.subScores.contains(QStringLiteral("scenes3d")));
        QCOMPARE(b.subScores.size(), 2);
        QVERIFY(f::score2d(r).has_value());  // the 2D groups are Ok
        for (SubResult &s : r.subResults) {
            if (s.id == QLatin1StringView("fill2d"))
                s.status = Status::Error;
        }
        QVERIFY(!f::score2d(r));
        QVERIFY(!f::score(r).subScores.contains(QStringLiteral("fill2d")));
    }
};

QTEST_GUILESS_MAIN(TestGpuFormula)
#include "tst_gpu_formula.moc"
