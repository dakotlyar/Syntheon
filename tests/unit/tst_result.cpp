// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/detail.h"
#include "core/enums.h"
#include "core/result.h"
#include "core/scoring.h"

#include <QTest>

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
