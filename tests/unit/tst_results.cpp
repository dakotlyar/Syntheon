// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/models/detail_blocks.h"
#include "app/models/log_filter_model.h"
#include "app/models/log_model.h"
#include "core/units.h"
#include "storage/run_repository.h"
#include "tests/common/stub_clock.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace sb;

namespace {

Metric num(const QString &key, const QString &label, double v, const QString &unit, bool headline = false)
{
    return {key, {}, label, v, unit, Direction::Neutral, headline};
}

TestResult batteryResult(double minutes, const QString &profile)
{
    TestResult r;
    r.testId = QStringLiteral("battery");
    r.status = Status::Ok;
    r.score = minutes;
    r.options = {{QStringLiteral("profile"), profile}};
    r.metrics = {num(QStringLiteral("runtime"), QStringLiteral("Battery life"), minutes, QStringLiteral("min"), true),
                 num(QStringLiteral("avg_drain"), QStringLiteral("Average drain"), 12.6, QStringLiteral("W")),
                 num(QStringLiteral("health"), QStringLiteral("Battery health"), 94, QStringLiteral("%"))};
    r.series = {{QStringLiteral("drain"), QStringLiteral("Charge over time"), QStringLiteral("Time"), QStringLiteral("min"),
                 QStringLiteral("%"), {{0, 100}, {minutes / 2, 50}, {minutes, 0}}}};
    return r;
}

TestResult cpuResult(double score)
{
    TestResult r;
    r.testId = QStringLiteral("cpu");
    r.status = Status::Ok;
    r.score = score;
    r.metrics = {num(QStringLiteral("single.mark"), QStringLiteral("Single-thread mark"), 3980, QStringLiteral("points"), true),
                 num(QStringLiteral("threads"), QStringLiteral("Threads used"), 16, QString()),
                 {QStringLiteral("simd.path"), {}, QStringLiteral("SIMD path"), QStringLiteral("AVX2"), {}, Direction::Neutral, false}};
    return r;
}

} // namespace

class TestResults : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<AppRuntime> m_rt;

    void startRuntime()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        AppRuntime::Options o;
        o.smoke = true;
        o.settingsFile = m_dir->filePath(QStringLiteral("settings.ini"));
        o.dataFolder = m_dir->filePath(QStringLiteral("data"));
        m_rt = std::make_unique<AppRuntime>(o);
        m_rt->storageController().open();
        m_rt->app().start();
        m_rt->run().refreshProbes();
        QTRY_VERIFY_WITH_TIMEOUT(!m_rt->run().probing(), 20000);
    }

    RunRecord seed(RunStatus status, const QDateTime &when, const QVector<TestResult> &tests, RunKind kind = RunKind::Standard)
    {
        RunRecord r;
        r.uuid = QUuid::createUuid();
        r.kind = kind;
        r.status = status;
        r.startedAt = when.toUTC();
        r.finishedAt = when.addSecs(60).toUTC();
        r.appVersion = QStringLiteral("2026.11.0");
        r.system.machineLabel = QStringLiteral("Knowhere · ASUS TUF Gaming A14");
        r.tests = tests;
        r.overall = computeOverall(tests, [](const QString &id) { return id != QLatin1StringView("battery"); });
        RunRepository repo(m_rt->storage().connection());
        [&] { QVERIFY(repo.saveRun(r)); }();
        return r;
    }

    static QStringList tabIds(const QVariantList &tabs)
    {
        QStringList ids;
        for (const QVariant &t : tabs)
            ids.append(t.toMap().value(QStringLiteral("id")).toString());
        return ids;
    }

    static QVariantMap block(const QVariantList &blocks, const QString &type, int nth = 0)
    {
        for (const QVariant &b : blocks) {
            if (b.toMap().value(QStringLiteral("type")).toString() == type && nth-- == 0)
                return b.toMap();
        }
        return {};
    }

private slots:
    void init() { tests::setStubTimeScale(0.03); }
    void cleanup()
    {
        m_rt.reset();
        m_dir.reset();
        tests::setStubTimeScale(0);
    }

    // --- the pure builder ---

    void blocksAreFormattedForQml()
    {
        TestResult r;
        r.testId = QStringLiteral("x");
        r.metrics = {num(QStringLiteral("a"), QStringLiteral("A"), 7012, QStringLiteral("MB/s")),
                     num(QStringLiteral("iops"), QStringLiteral("I"), 181000, QStringLiteral("IOPS")),
                     num(QStringLiteral("lat"), QStringLiteral("Lat"), 1840, QStringLiteral("µs")),
                     {QStringLiteral("path"), {}, QStringLiteral("Path"), QStringLiteral("AVX2"), {}, Direction::Neutral, false}};
        r.series = {{QStringLiteral("c"), QStringLiteral("C"), {}, {}, QStringLiteral("ns"), {{4, 1}, {1024, 10}, {1048576, 70}}}};

        DetailLayout layout;
        KpisBlock kpis;
        kpis.items = {{QStringLiteral("Read"), DetailCell::metric(QStringLiteral("a")), {}},
                      {QStringLiteral("Path"), DetailCell::metric(QStringLiteral("path")), {}},
                      {QStringLiteral("Missing"), DetailCell::metric(QStringLiteral("nope")), {}},
                      {QStringLiteral("Text"), DetailCell::literal(QStringLiteral("hello")), QStringLiteral("things")}};
        layout.blocks.append({{}, kpis});
        TableBlock table;
        table.columns = {QStringLiteral("Metric"), QStringLiteral("Value")};
        table.rows = {{DetailCell::literal(QStringLiteral("Lat")), DetailCell::metric(QStringLiteral("lat"))}};
        layout.blocks.append({QStringLiteral("T"), table});
        GridBlock grid;
        grid.columnLabels = {QStringLiteral("Read")};
        grid.rowLabels = {QStringLiteral("Random 4K")};
        grid.rowSmallText = {QStringLiteral("Q32T1")};
        grid.cells = {{{QStringLiteral("a"), QStringLiteral("iops")}}};
        layout.blocks.append({{}, grid});
        LineChartBlock chart;
        chart.seriesKeys = {QStringLiteral("c")};
        chart.xAxis.scale = ChartAxis::Scale::Log2;
        chart.xAxis.tickLabels = {QStringLiteral("4K"), QStringLiteral("1G")};
        chart.xAxis.tickValues = {4, 1048576};
        chart.yUnit = QStringLiteral("ns");
        chart.annotations = {{1024, QStringLiteral("L2")}};
        layout.blocks.append({QStringLiteral("Chart"), chart});
        layout.blocks.append({{}, NoteBlock{QStringLiteral("careful"), Tone::Warn}});
        layout.blocks.append({{}, NaStateBlock{QStringLiteral("No NPU"), QStringLiteral("because")}});
        layout.blocks.append({{}, CustomBlock{QStringLiteral("Fancy"), {{QStringLiteral("k"), 1}}}});

        const QVariantList out = buildDetailBlocks(layout, r, QStringLiteral("t-ram"));
        QCOMPARE(out.size(), 7);

        const auto items = block(out, QStringLiteral("kpis")).value(QStringLiteral("items")).toList();
        QCOMPARE(items[0].toMap().value(QStringLiteral("value")).toString(), QStringLiteral("7,012"));
        QCOMPARE(items[0].toMap().value(QStringLiteral("unit")).toString(), QStringLiteral("MB/s"));
        QCOMPARE(items[1].toMap().value(QStringLiteral("value")).toString(), QStringLiteral("AVX2"));
        QCOMPARE(items[2].toMap().value(QStringLiteral("value")).toString(), QStringLiteral("—"));  // a missing metric
        QCOMPARE(items[3].toMap().value(QStringLiteral("unit")).toString(), QStringLiteral("things"));

        const auto t = block(out, QStringLiteral("table"));
        QCOMPARE(t.value(QStringLiteral("title")).toString(), QStringLiteral("T"));
        QCOMPARE(t.value(QStringLiteral("rows")).toList()[0].toStringList(), (QStringList{QStringLiteral("Lat"), QStringLiteral("1,840 µs")}));

        const auto g = block(out, QStringLiteral("grid"));
        const auto cell = g.value(QStringLiteral("rows")).toList()[0].toMap().value(QStringLiteral("cells")).toList()[0].toMap();
        QCOMPARE(cell.value(QStringLiteral("main")).toString(), QStringLiteral("7,012"));
        QCOMPARE(cell.value(QStringLiteral("sub")).toString(), QStringLiteral("MB/s · 181k IOPS"));

        const auto c = block(out, QStringLiteral("linechart"));
        QCOMPARE(c.value(QStringLiteral("points")).toList().size(), 3);
        QCOMPARE(c.value(QStringLiteral("logX")).toBool(), true);
        QCOMPARE(c.value(QStringLiteral("xTicks")).toList()[1].toMap().value(QStringLiteral("x")).toDouble(), 1048576.0);
        QCOMPARE(c.value(QStringLiteral("colorToken")).toString(), QStringLiteral("t-ram"));
        QCOMPARE(block(out, QStringLiteral("note")).value(QStringLiteral("tone")).toString(), QStringLiteral("warn"));
        QCOMPARE(block(out, QStringLiteral("nastate")).value(QStringLiteral("body")).toString(), QStringLiteral("because"));
        QCOMPARE(block(out, QStringLiteral("custom")).value(QStringLiteral("qmlType")).toString(), QStringLiteral("Fancy"));

        QCOMPARE(formatCompactIops(999), QStringLiteral("999 IOPS"));
        QCOMPARE(formatCompactIops(2500000), QStringLiteral("2.5M IOPS"));
        // Tick labels without positions spread evenly.
        layout.blocks.clear();
        LineChartBlock spread = chart;
        spread.xAxis.tickValues.clear();
        spread.xAxis.tickLabels = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
        layout.blocks.append({{}, spread});
        const auto ticks = buildDetailBlocks(layout, r, {}).first().toMap().value(QStringLiteral("xTicks")).toList();
        QCOMPARE(ticks[1].toMap().value(QStringLiteral("x")).toDouble(), (4 + 1048576) / 2.0);
    }

    // --- the controller ---

    void summaryOfAStoredRun()
    {
        startRuntime();
        QVERIFY(!m_rt->results().valid());
        const QDateTime t0 = QDateTime(QDate(2026, 10, 15), QTime(14, 2), QTimeZone::UTC);
        const RunRecord run = seed(RunStatus::Completed, t0, {cpuResult(1284), [] {
                                       TestResult na = TestResult::notAvailable(QStringLiteral("No supported NPU found"));
                                       na.testId = QStringLiteral("npu");
                                       return na;
                                   }()});
        ResultsController &res = m_rt->results();
        QVERIFY(res.openRun(run.uuid.toString(QUuid::WithoutBraces)));
        QVERIFY(res.valid());
        QCOMPARE(res.title(), QStringLiteral("Results"));
        QVERIFY2(res.subtitle().contains(QStringLiteral("Knowhere · ASUS TUF Gaming A14")), qPrintable(res.subtitle()));
        QVERIFY(res.subtitle().contains(QLatin1StringView("2 tests")));
        QCOMPARE(res.overallText(), QStringLiteral("1,284"));
        QVERIFY(!res.overallPartial());
        QCOMPARE(res.statusLabel(), QString());
        QCOMPARE(res.scoredFraction(), 0.5);
        QCOMPARE(res.rows().size(), 2);
        const QVariantMap cpu = res.rows()[0].toMap(), npu = res.rows()[1].toMap();
        QCOMPARE(cpu.value(QStringLiteral("score")).toString(), QStringLiteral("1,284"));
        QVERIFY(cpu.value(QStringLiteral("raw")).toString().contains(QLatin1StringView("Single-thread mark 3,980")));
        QCOMPARE(npu.value(QStringLiteral("score")).toString(), QStringLiteral("N/A"));
        QCOMPARE(npu.value(QStringLiteral("raw")).toString(), QStringLiteral("No supported NPU found"));
        QVERIFY(npu.value(QStringLiteral("na")).toBool());
        // The newest run opens when no id is given.
        QVERIFY(res.openRun({}));
        QCOMPARE(res.uuid(), run.uuid.toString(QUuid::WithoutBraces));
        QVERIFY(!res.openRun(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    }

    void stoppedRunIsMarkedPartial()
    {
        startRuntime();
        const RunRecord stopped = seed(RunStatus::Stopped, QDateTime::currentDateTimeUtc(), {cpuResult(1284), [] {
                                           TestResult r = TestResult::withStatus(Status::NotRun, QStringLiteral("Not run: the run was stopped"));
                                           r.testId = QStringLiteral("ram");
                                           return r;
                                       }()});
        ResultsController &res = m_rt->results();
        QVERIFY(res.openRun(stopped.uuid.toString(QUuid::WithoutBraces)));
        QCOMPARE(res.statusLabel(), QStringLiteral("Stopped"));
        QVERIFY(res.overallPartial());  // N2
        QVERIFY(res.overallNote().startsWith(QLatin1StringView("Partial")));
        QCOMPARE(res.overallText(), QStringLiteral("1,284"));
        QCOMPARE(res.rows()[1].toMap().value(QStringLiteral("score")).toString(), QStringLiteral("Not run"));

        const RunRecord interrupted = seed(RunStatus::Interrupted, QDateTime::currentDateTimeUtc().addSecs(5), {cpuResult(100)});
        QVERIFY(res.openRun(interrupted.uuid.toString(QUuid::WithoutBraces)));
        QCOMPARE(res.statusLabel(), QStringLiteral("Interrupted"));
        QVERIFY(res.overallNote().contains(QLatin1StringView("interrupted")));
        const RunRecord recovered = seed(RunStatus::Recovered, QDateTime::currentDateTimeUtc().addSecs(10), {batteryResult(402, QStringLiteral("mixed"))}, RunKind::Exclusive);
        QVERIFY(res.openRun(recovered.uuid.toString(QUuid::WithoutBraces)));
        QCOMPARE(res.statusLabel(), QStringLiteral("Recovered"));
        QVERIFY(!res.overallPartial());
    }

    // D4 + N4
    void batteryTabShowsTheNewestBatteryRunOnOrBefore()
    {
        startRuntime();
        const QDateTime day1 = QDateTime(QDate(2026, 10, 1), QTime(9, 0), QTimeZone::UTC);
        const QDateTime day5 = day1.addDays(4), day9 = day1.addDays(8), day12 = day1.addDays(11);
        const RunRecord early = seed(RunStatus::Completed, day1.addDays(-1), {cpuResult(900)});
        const RunRecord bat1 = seed(RunStatus::Completed, day1, {batteryResult(300, QStringLiteral("light"))}, RunKind::Exclusive);
        const RunRecord mid = seed(RunStatus::Completed, day5, {cpuResult(1000)});
        const RunRecord bat2 = seed(RunStatus::Completed, day9, {batteryResult(402, QStringLiteral("mixed"))}, RunKind::Exclusive);
        const RunRecord late = seed(RunStatus::Completed, day12, {cpuResult(1100)});
        ResultsController &res = m_rt->results();
        const auto open = [&](const RunRecord &r) { return res.openRun(r.uuid.toString(QUuid::WithoutBraces)); };

        // Before any Battery run: no Battery tab.
        QVERIFY(open(early));
        QCOMPARE(tabIds(res.detailTabs()), QStringList{QStringLiteral("cpu")});
        // A run between the two sees the earlier Battery run, not the newer.
        QVERIFY(open(mid));
        QCOMPARE(tabIds(res.detailTabs()), (QStringList{QStringLiteral("cpu"), QStringLiteral("battery")}));
        QVERIFY(res.detailTabs().last().toMap().value(QStringLiteral("separate")).toBool());
        res.selectDetail(QStringLiteral("battery"));
        QVERIFY2(res.detailNote().startsWith(QStringLiteral("From a separate run · Light profile · 1 Oct 2026")), qPrintable(res.detailNote()));
        QCOMPARE(block(res.detailBlocks(), QStringLiteral("kpis")).value(QStringLiteral("items")).toList()[0].toMap()
                     .value(QStringLiteral("value")).toString(), QStringLiteral("300"));
        // The latest run sees the newer one.
        QVERIFY(open(late));
        res.selectDetail(QStringLiteral("battery"));
        QVERIFY(res.detailNote().contains(QStringLiteral("Mixed profile · 9 Oct 2026")));
        // Viewing a Battery run itself: its own tab, no separate one.
        QVERIFY(open(bat2));
        QCOMPARE(tabIds(res.detailTabs()), QStringList{QStringLiteral("battery")});
        QVERIFY(!res.detailTabs()[0].toMap().value(QStringLiteral("separate")).toBool());
        res.selectDetail(QStringLiteral("battery"));
        QVERIFY(res.detailNote().isEmpty());
    }

    void detailPageForARealStubRun()
    {
        startRuntime();
        RunController &rc = m_rt->run();
        for (const QString &id : {QStringLiteral("cpu"), QStringLiteral("ram"), QStringLiteral("storage"), QStringLiteral("gpu"), QStringLiteral("ai")})
            rc.toggle(id);
        QSignalSpy finished(&rc, &RunController::runFinished);
        QVERIFY(rc.start());
        QVERIFY(finished.wait(60000));
        ResultsController &res = m_rt->results();
        // The finished run replaced the live view automatically.
        QCOMPARE(res.uuid(), finished.last().at(0).toString());
        QVERIFY(!res.live());
        QCOMPARE(res.rows().size(), 5);

        res.showDetail(QStringLiteral("ram"));
        QVERIFY(res.detailOpen());
        QCOMPARE(res.detailTitle(), QStringLiteral("RAM"));
        QVERIFY(res.detailSubtitle().startsWith(QLatin1StringView("Score ")));
        QVERIFY(!block(res.detailBlocks(), QStringLiteral("kpis")).isEmpty());
        QVERIFY(!block(res.detailBlocks(), QStringLiteral("linechart")).value(QStringLiteral("points")).toList().isEmpty());
        QCOMPARE(res.detailColorToken(), QStringLiteral("t-ram"));
        res.showDetail(QStringLiteral("storage"));
        const auto grid = block(res.detailBlocks(), QStringLiteral("grid"));
        QCOMPARE(grid.value(QStringLiteral("rows")).toList().size(), 4);
        res.showDetail(QStringLiteral("ai"));
        QVERIFY(!block(res.detailBlocks(), QStringLiteral("table")).isEmpty());
        res.closeDetail();
        QVERIFY(!res.detailOpen());

        // D16: the log was saved with the run and reads back, filterable.
        auto *log = qobject_cast<LogFilterModel *>(res.logModel());
        QVERIFY(log);
        QVERIFY(log->rowCount() >= 10);
        const int all = log->rowCount();
        log->setTestFilter(QStringLiteral("ram"));
        QVERIFY(log->rowCount() > 0 && log->rowCount() < all);
        QVERIFY(log->visibleText().contains(QLatin1StringView("RAM")));
        QVERIFY(!log->visibleText().contains(QLatin1StringView("CPU")));
        log->setTestFilter({});
        log->setMinLevel(QStringLiteral("warning"));
        QVERIFY(log->rowCount() < all);
        QCOMPARE(res.logTests().size(), 5);
    }

    void followsTheLiveRun()
    {
        tests::setStubTimeScale(0.3);
        startRuntime();
        RunController &rc = m_rt->run();
        rc.toggle(QStringLiteral("cpu"));
        rc.toggle(QStringLiteral("ram"));
        QSignalSpy finished(&rc, &RunController::runFinished);
        QVERIFY(rc.start());
        ResultsController &res = m_rt->results();
        QTRY_VERIFY_WITH_TIMEOUT(rc.running(), 5000);
        res.openCurrent();  // "View results so far"
        QVERIFY(res.live());
        QCOMPARE(res.title(), QStringLiteral("Results so far"));
        // It updates as tests finish; the overall so far is partial.
        QTRY_VERIFY_WITH_TIMEOUT(res.rows().size() >= 1 && res.rows().first().toMap().value(QStringLiteral("status")).toString() == QLatin1StringView("ok"), 40000);
        QVERIFY(res.overallPartial());
        QVERIFY(res.overallNote().startsWith(QStringLiteral("Partial")));
        rc.stop();
        QVERIFY(finished.wait(20000));
        QVERIFY(!res.live());  // switched to the stored copy
        QCOMPARE(res.statusLabel(), QStringLiteral("Stopped"));
    }

    void clearingHistoryResetsTheView()
    {
        startRuntime();
        seed(RunStatus::Completed, QDateTime::currentDateTimeUtc(), {cpuResult(500)});
        ResultsController &res = m_rt->results();
        QVERIFY(res.openRun({}));
        QVERIFY(res.valid());
        QCOMPARE(m_rt->storageController().clearHistory(), QString());
        QVERIFY(!res.valid());
        QVERIFY(res.rows().isEmpty());
        QVERIFY(res.detailBlocks().isEmpty());
    }
};

QTEST_MAIN(TestResults)
#include "tst_results.moc"
