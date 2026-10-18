// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/detail.h"
#include "core/enums.h"
#include "core/result.h"
#include "core/scoring.h"

#include <QTest>

#include <cmath>

using namespace sb;

namespace {

TestResult sampleResult()
{
    TestResult r;
    r.testId = QStringLiteral("ram");
    r.metrics = {
        {QStringLiteral("bandwidth.read"), {}, QStringLiteral("Read bandwidth"), 78.4, QStringLiteral("GB/s"),
         Direction::HigherIsBetter, true},
        {QStringLiteral("latency"), {}, QStringLiteral("Latency"), 74.2, QStringLiteral("ns"), Direction::LowerIsBetter,
         true},
        {QStringLiteral("timings"), {}, QStringLiteral("Timings"), QStringLiteral("40-40-40-77"), QString(),
         Direction::Neutral, false},
    };
    r.series = {{QStringLiteral("latency_curve"), QStringLiteral("Latency curve"), QStringLiteral("Block size"),
                 QStringLiteral("KiB"), QStringLiteral("ns"), {{4, 1.1}, {8, 1.2}}}};
    return r;
}

TestResult scored(const QString &id, std::optional<double> score)
{
    TestResult r;
    r.testId = id;
    r.score = score;
    return r;
}

} // namespace

class TestResultTypes : public QObject
{
    Q_OBJECT

private slots:
    // Z4: any N/A result gets the N/A block with its reason; an NPU-style
    // layout that has its own N/A block, and other statuses, are unchanged.
    void notAvailableDetailState()
    {
        DetailLayout normal;
        normal.subtitle = QStringLiteral("— · — channels");
        normal.blocks.append({{}, NoteBlock{QStringLiteral("x"), Tone::Neutral}});
        TestResult na = TestResult::notAvailable(QStringLiteral("No DIMM data"));
        const DetailLayout d = withNotAvailableState(normal, na, QStringLiteral("RAM"));
        QCOMPARE(d.subtitle, QStringLiteral("Not available on this machine"));
        QCOMPARE(d.blocks.size(), 1);
        const auto *b = std::get_if<NaStateBlock>(&d.blocks[0].content);
        QVERIFY(b);
        QCOMPARE(b->title, QStringLiteral("RAM isn't available here"));
        QCOMPARE(b->body, QStringLiteral("No DIMM data"));

        DetailLayout own;
        own.blocks.append({{}, NaStateBlock{QStringLiteral("No supported NPU found"), QStringLiteral("why")}});
        QCOMPARE(withNotAvailableState(own, na, QStringLiteral("NPU")).blocks, own.blocks);

        TestResult ok;
        ok.status = Status::Ok;
        QCOMPARE(withNotAvailableState(normal, ok, QStringLiteral("RAM")).blocks, normal.blocks);
        TestResult error = TestResult::error(QStringLiteral("boom"));
        QCOMPARE(withNotAvailableState(normal, error, QStringLiteral("RAM")).blocks, normal.blocks);
    }

    void statusStringsRoundTrip()
    {
        const Status all[] = {Status::Ok,        Status::Partial,  Status::NotAvailable, Status::Error,
                              Status::Cancelled, Status::TimedOut, Status::Interrupted,  Status::NotRun};
        QSet<QString> seen;
        for (Status s : all) {
            const QString name = toKey(s);
            QVERIFY(!name.isEmpty());
            QCOMPARE(name, name.toLower());
            QVERIFY(!seen.contains(name));
            seen.insert(name);
            QVERIFY(statusFromKey(name) == std::optional<Status>(s));
        }
        QVERIFY(statusFromKey(QStringLiteral("OK")) == std::nullopt);
    }

    void stableStoredNames()
    {
        // These strings are the stored format; changing one breaks old databases.
        QCOMPARE(toKey(Status::NotAvailable), QStringLiteral("not_available"));
        QCOMPARE(toKey(Status::TimedOut), QStringLiteral("timed_out"));
        QCOMPARE(toKey(Status::NotRun), QStringLiteral("not_run"));
        QCOMPARE(toKey(RunKind::Exclusive), QStringLiteral("exclusive"));
        QCOMPARE(toKey(RunStatus::Stopped), QStringLiteral("stopped"));
        QCOMPARE(toKey(RunStatus::Recovered), QStringLiteral("recovered"));
        QCOMPARE(toKey(Direction::LowerIsBetter), QStringLiteral("lower"));
        QCOMPARE(toKey(LogLevel::Warning), QStringLiteral("warning"));
    }

    void otherEnumRoundTrips()
    {
        for (RunStatus s : {RunStatus::Running, RunStatus::Completed, RunStatus::Stopped, RunStatus::Interrupted,
                            RunStatus::Recovered})
            QVERIFY(runStatusFromKey(toKey(s)) == std::optional<RunStatus>(s));
        for (RunKind k : {RunKind::Standard, RunKind::Exclusive})
            QVERIFY(runKindFromKey(toKey(k)) == std::optional<RunKind>(k));
        for (Direction d : {Direction::HigherIsBetter, Direction::LowerIsBetter, Direction::Neutral})
            QVERIFY(directionFromKey(toKey(d)) == std::optional<Direction>(d));
        for (LogLevel l : {LogLevel::Debug, LogLevel::Info, LogLevel::Warning, LogLevel::Error})
            QVERIFY(logLevelFromKey(toKey(l)) == std::optional<LogLevel>(l));
        for (Tone t : {Tone::Neutral, Tone::Good, Tone::Warn, Tone::Bad, Tone::Na})
            QVERIFY(toneFromKey(toKey(t)) == std::optional<Tone>(t));
    }

    void metricLookup()
    {
        const TestResult r = sampleResult();
        QVERIFY(r.metric(QStringLiteral("latency")));
        QVERIFY(r.metric(QStringLiteral("latency"))->isNumeric());
        QVERIFY(!r.metric(QStringLiteral("timings"))->isNumeric());
        QVERIFY(!r.metric(QStringLiteral("missing")));
    }

    void factories()
    {
        QCOMPARE(TestResult::interrupted().status, Status::Interrupted);
        const TestResult na = TestResult::notAvailable(QStringLiteral("No supported NPU found"));
        QCOMPARE(na.status, Status::NotAvailable);
        QCOMPARE(na.statusReason, QStringLiteral("No supported NPU found"));
        QVERIFY(!na.score);
        QCOMPARE(TestResult::error(QStringLiteral("boom")).status, Status::Error);
    }

    void overallSumsCountedScores()
    {
        const QVector<TestResult> tests = {scored(QStringLiteral("cpu"), 1284), scored(QStringLiteral("ram"), 410.5),
                                           scored(QStringLiteral("npu"), std::nullopt),
                                           scored(QStringLiteral("battery"), 900)};
        const auto counts = [](const QString &id) { return id != QLatin1StringView("battery"); };
        QCOMPARE(computeOverall(tests, counts), std::optional<double>(1694.5));
    }

    void overallEmptyWhenNoScores()
    {
        const QVector<TestResult> tests = {scored(QStringLiteral("npu"), std::nullopt)};
        QCOMPARE(computeOverall(tests, [](const QString &) { return true; }), std::nullopt);
        QCOMPARE(computeOverall({}, [](const QString &) { return true; }), std::nullopt);
    }

    void weightedScoreFormula()
    {
        // round(C × Σ wᵢ·mᵢ) = round(10 × (0.5×78.4 + 0.25×74.2)) = round(577.5) = 578
        const auto s = weightedScore(sampleResult(), 10.0,
                                     {{QStringLiteral("bandwidth.read"), 0.5}, {QStringLiteral("latency"), 0.25}});
        QCOMPARE(s, std::optional<double>(578.0));
    }

    void weightedScoreDeterministic()
    {
        const QVector<ScoreWeight> w = {{QStringLiteral("bandwidth.read"), 0.7}, {QStringLiteral("latency"), -0.3}};
        const auto first = weightedScore(sampleResult(), 3.3, w);
        for (int i = 0; i < 100; ++i)
            QCOMPARE(weightedScore(sampleResult(), 3.3, w), first);
    }

    // §7.2 / §7.3: the weighted geometric mean, with the design's worked examples.
    void weightedGeometricScoreFormula()
    {
        const auto result = [](std::initializer_list<std::pair<const char *, double>> values) {
            TestResult r;
            for (const auto &[key, v] : values)
                r.metrics.append({QLatin1StringView(key), {}, QString(), v, QString(), Direction::HigherIsBetter, false});
            return r;
        };
        // Seven groups at the design's weights, one metric each, anchors 100.
        const char *const ids[] = {"int", "fp", "simd", "compress", "crypto", "sort", "physics"};
        const double w[] = {0.25, 0.25, 0.15, 0.10, 0.10, 0.10, 0.05};
        QVector<GeometricGroup> groups;
        for (int i = 0; i < 7; ++i)
            groups.append({QLatin1StringView(ids[i]), w[i], {{QLatin1StringView(ids[i]), 100.0, 1.0}}});
        // Everything at the anchors: 1,000; SIMD at half: 1000 x 0.5^0.15 = 901; all at 0.6: 600.
        auto all = [&](double f, double simd) {
            return result({{"int", 100 * f}, {"fp", 100 * f}, {"simd", 100 * simd}, {"compress", 100 * f},
                           {"crypto", 100 * f}, {"sort", 100 * f}, {"physics", 100 * f}});
        };
        QCOMPARE(weightedGeometricScore(all(1, 1), 1000, groups).total, std::optional<double>(1000));
        QCOMPARE(weightedGeometricScore(all(1, 0.5), 1000, groups).total, std::optional<double>(901));
        QCOMPARE(weightedGeometricScore(all(0.6, 0.6), 1000, groups).total, std::optional<double>(600));
        const GeometricScore half = weightedGeometricScore(all(1, 0.5), 1000, groups);
        QCOMPARE(half.groups.value(QStringLiteral("simd")), 500.0);
        QCOMPARE(half.groups.value(QStringLiteral("int")), 1000.0);
        // Ratios don't depend on the anchors: doubling one anchor scales every result alike.
        QVector<GeometricGroup> other = groups;
        other[2].metrics[0].anchor = 200;
        const double a = *weightedGeometricScore(all(1, 1), 1000, other).total;
        const double b = *weightedGeometricScore(all(0.6, 0.6), 1000, other).total;
        QVERIFY(qAbs(b / a - 0.6) < 0.002);
        // Inside a group: 80/20 of 2x and 1x = 2^0.8.
        const QVector<GeometricGroup> fp = {{QStringLiteral("fp"), 1.0, {{QStringLiteral("a"), 1, 0.8}, {QStringLiteral("b"), 1, 0.2}}}};
        QCOMPARE(weightedGeometricScore(result({{"a", 2}, {"b", 1}}), 1000, fp).total, std::optional<double>(std::round(1000 * std::pow(2, 0.8))));
    }

    // A missing, text or non-positive metric: that group has no score and
    // the total is missing; the other groups keep theirs.
    void weightedGeometricScoreMissingGroup()
    {
        TestResult r = sampleResult();
        const QVector<GeometricGroup> groups = {
            {QStringLiteral("bw"), 0.5, {{QStringLiteral("bandwidth.read"), 78.4, 1.0}}},
            {QStringLiteral("gone"), 0.5, {{QStringLiteral("missing"), 1.0, 1.0}}},
        };
        GeometricScore s = weightedGeometricScore(r, 1000, groups);
        QVERIFY(!s.total);
        QCOMPARE(s.groups.value(QStringLiteral("bw")), 1000.0);
        QVERIFY(!s.groups.contains(QStringLiteral("gone")));
        const QVector<GeometricGroup> text = {{QStringLiteral("t"), 1.0, {{QStringLiteral("timings"), 1.0, 1.0}}}};
        QVERIFY(!weightedGeometricScore(r, 1000, text).total);
        r.metrics[0].value = 0.0;
        QVERIFY(!weightedGeometricScore(r, 1000, {groups[0]}).total);
        QVERIFY(!weightedGeometricScore(r, 1000, {}).total);
    }

    void weightedScoreMissingOrTextMetric()
    {
        QCOMPARE(weightedScore(sampleResult(), 1.0, {{QStringLiteral("missing"), 1.0}}), std::nullopt);
        QCOMPARE(weightedScore(sampleResult(), 1.0, {{QStringLiteral("timings"), 1.0}}), std::nullopt);
    }

    void snapshotJsonRoundTrip()
    {
        SystemSnapshot s;
        s.machineLabel = QStringLiteral("Tiamat");
        s.data.insert(QStringLiteral("cpu"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Ryzen 9")}});
        const QJsonObject json = s.toJson();
        QCOMPARE(json.value(QStringLiteral("sysinfo_version")).toInt(), SystemSnapshot::kCurrentVersion);
        QCOMPARE(json.value(QStringLiteral("machine_label")).toString(), QStringLiteral("Tiamat"));
        QCOMPARE(SystemSnapshot::fromJson(json), s);
    }

    void genericLayout()
    {
        TestResult r = sampleResult();
        r.score = 1284;
        const DetailLayout layout = genericDetailLayout(r);
        QCOMPARE(layout.subtitle, QStringLiteral("Score 1,284"));
        QCOMPARE(layout.headlineMetrics, (QStringList{QStringLiteral("bandwidth.read"), QStringLiteral("latency")}));
        QCOMPARE(layout.blocks.size(), 3);

        const auto *kpis = std::get_if<KpisBlock>(&layout.blocks[0].content);
        QVERIFY(kpis);
        QCOMPARE(kpis->items.size(), 2);
        QCOMPARE(kpis->items[0].value.metricKey, QStringLiteral("bandwidth.read"));

        const auto *table = std::get_if<TableBlock>(&layout.blocks[1].content);
        QVERIFY(table);
        QCOMPARE(table->rows.size(), 3);
        QCOMPARE(table->rows[2][0].text, QStringLiteral("Timings"));
        QCOMPARE(table->rows[2][1].metricKey, QStringLiteral("timings"));

        const auto *chart = std::get_if<LineChartBlock>(&layout.blocks[2].content);
        QVERIFY(chart);
        QCOMPARE(chart->seriesKeys, QStringList{QStringLiteral("latency_curve")});
        QCOMPARE(layout.blocks[2].title, QStringLiteral("Latency curve"));
    }

    // A marker series (window starts, Phase 2's telemetry.windows) is never
    // plotted as if it were a measurement.
    void genericLayoutSkipsMarkerSeries()
    {
        TestResult r = sampleResult();
        r.series.append({QStringLiteral("telemetry.windows"), QStringLiteral("Measurement windows"), QStringLiteral("Time"),
                         QStringLiteral("s"), QString(), {{0, 1}, {20, 10}}});
        QVERIFY(isMarkerSeries(r.series.last()));
        QVERIFY(!isMarkerSeries(r.series.first()));
        const DetailLayout layout = genericDetailLayout(r);
        int charts = 0;
        for (const DetailBlock &b : layout.blocks)
            charts += std::holds_alternative<LineChartBlock>(b.content) ? 1 : 0;
        QCOMPARE(charts, 1);
    }

    void genericLayoutForNotAvailable()
    {
        const DetailLayout layout = genericDetailLayout(TestResult::notAvailable(QStringLiteral("No supported NPU")));
        QVERIFY(layout.subtitle.isEmpty());
        QCOMPARE(layout.blocks.size(), 1);
        const auto *note = std::get_if<NoteBlock>(&layout.blocks[0].content);
        QVERIFY(note);
        QCOMPARE(note->text, QStringLiteral("No supported NPU"));
    }
};

QTEST_GUILESS_MAIN(TestResultTypes)
#include "tst_result.moc"
