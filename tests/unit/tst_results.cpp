// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/controllers/compare_controller.h"
#include "app/models/detail_blocks.h"
#include "app/models/history_model.h"
#include "app/models/log_filter_model.h"
#include "app/models/log_model.h"
#include "core/units.h"
#include "storage/database.h"
#include "storage/run_repository.h"
#include "tests/common/stub_clock.h"

#include <QSignalSpy>
#include <limits>
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
        m_rt->startup().waitForRecovery();
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

    // Phase 3 D13 and the RAM mockup: LineChartBlock::yAxis and markers.
    // The default (Linear, no markers) gives the chart exactly what it got
    // before (no y ticks: it lays out its own five lines; no markers); a log
    // y axis passes its ticks and labels, or picks 1-2-5 / powers of two
    // over the data; markers are the curve's points at the given x.
    void lineChartYAxisAndMarkers()
    {
        TestResult r;
        r.series = {{QStringLiteral("c"), QStringLiteral("C"), {}, {}, QStringLiteral("ns"),
                     {{4, 0.8}, {24, 0.9}, {220, 2.6}, {5836.8, 9.8}, {1048576, 92}}}};
        LineChartBlock chart;
        chart.seriesKeys = {QStringLiteral("c")};
        chart.xAxis.scale = ChartAxis::Scale::Log2;
        QCOMPARE(chart.yAxis.scale, ChartAxis::Scale::Linear);
        QVERIFY(chart.markers.isEmpty());
        const auto build = [&](const LineChartBlock &b) {
            DetailLayout layout;
            layout.blocks.append({QStringLiteral("Latency"), b});
            return buildDetailBlocks(layout, r, QStringLiteral("t-ram")).first().toMap();
        };
        const QVariantMap plain = build(chart);
        QCOMPARE(plain.value(QStringLiteral("logY")).toBool(), false);
        QVERIFY(plain.value(QStringLiteral("yTicks")).toList().isEmpty());
        QVERIFY(plain.value(QStringLiteral("markers")).toList().isEmpty());
        QCOMPARE(plain.value(QStringLiteral("logX")).toBool(), true);

        // The mockup's axis: given ticks and labels.
        LineChartBlock log = chart;
        log.yAxis.scale = ChartAxis::Scale::Log10;
        log.yAxis.tickValues = {0.5, 1, 2, 5, 10, 20, 50, 100, 200};
        log.yAxis.tickLabels = {QStringLiteral("0.5"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("5"),
                                QStringLiteral("10"), QStringLiteral("20"), QStringLiteral("50"), QStringLiteral("100"),
                                QStringLiteral("200")};
        log.markers = {24, 220, 5836.8, 3000};  // 3000: no point there, skipped
        const QVariantMap m = build(log);
        QCOMPARE(m.value(QStringLiteral("logY")).toBool(), true);
        const QVariantList yt = m.value(QStringLiteral("yTicks")).toList();
        QCOMPARE(yt.size(), 9);
        QCOMPARE(yt.first().toMap().value(QStringLiteral("y")).toDouble(), 0.5);
        QCOMPARE(yt.last().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("200"));
        const QVariantList mk = m.value(QStringLiteral("markers")).toList();
        QCOMPARE(mk.size(), 3);
        QCOMPARE(mk[1].toMap().value(QStringLiteral("x")).toDouble(), 220.0);
        QCOMPARE(mk[1].toMap().value(QStringLiteral("y")).toDouble(), 2.6);

        // No ticks given: 1-2-5 from below the lowest point to above the highest.
        log.yAxis.tickValues.clear();
        log.yAxis.tickLabels.clear();
        QStringList labels;
        for (const QVariant &t : build(log).value(QStringLiteral("yTicks")).toList())
            labels.append(t.toMap().value(QStringLiteral("label")).toString());
        QCOMPARE(labels, (QStringList{QStringLiteral("0.5"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("5"),
                                      QStringLiteral("10"), QStringLiteral("20"), QStringLiteral("50"), QStringLiteral("100")}));
        log.yAxis.scale = ChartAxis::Scale::Log2;
        labels.clear();
        for (const QVariant &t : build(log).value(QStringLiteral("yTicks")).toList())
            labels.append(t.toMap().value(QStringLiteral("label")).toString());
        QCOMPARE(labels, (QStringList{QStringLiteral("0.5"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("4"),
                                      QStringLiteral("8"), QStringLiteral("16"), QStringLiteral("32"), QStringLiteral("64"),
                                      QStringLiteral("128")}));
        // AP-5 L7: an infinite or denormal point gives no automatic ticks
        // (and returns: no endless loop), for both sequences.
        for (const double bad : {double(INFINITY), 5e-324}) {
            r.series[0].points.append({2097152, bad});
            for (const ChartAxis::Scale scale : {ChartAxis::Scale::Log2, ChartAxis::Scale::Log10}) {
                log.yAxis.scale = scale;
                QVERIFY(build(log).value(QStringLiteral("yTicks")).toList().isEmpty());
            }
            r.series[0].points.removeLast();
        }
    }

    // --- the controller ---

    // Phase 3 (Dave, 2026-10-20): sortByX draws a series stored in another
    // order (the RAM test's loaded latency, by load level) by x; off, the
    // stored order is kept.
    void lineChartSortByX()
    {
        TestResult r;
        r.series = {{QStringLiteral("c"), QStringLiteral("C"), {}, {}, QStringLiteral("ns"),
                     {{0, 76}, {15.1, 85}, {41.5, 105}, {66.2, 161}, {64.0, 182}, {64.1, 197}}}};
        LineChartBlock chart;
        chart.seriesKeys = {QStringLiteral("c")};
        chart.annotations = {{0, QStringLiteral("Idle 76 ns")}, {64.1, QStringLiteral("Full load 197 ns")}};
        const auto xs = [&](const LineChartBlock &b) {
            DetailLayout layout;
            layout.blocks.append({QStringLiteral("Loaded"), b});
            const QVariantMap m = buildDetailBlocks(layout, r, QStringLiteral("t-ram")).first().toMap();
            QList<double> out;
            for (const QVariant &p : m.value(QStringLiteral("points")).toList())
                out << p.toMap().value(QStringLiteral("x")).toDouble();
            return out;
        };
        QCOMPARE(xs(chart), (QList<double>{0, 15.1, 41.5, 66.2, 64.0, 64.1}));
        chart.sortByX = true;
        QCOMPARE(xs(chart), (QList<double>{0, 15.1, 41.5, 64.0, 64.1, 66.2}));
        // AP-5 L1: the filled dot on the full-load point, not the last one.
        chart.filledAt = 64.1;
        DetailLayout layout;
        layout.blocks.append({QStringLiteral("Loaded"), chart});
        const QVariantMap m = buildDetailBlocks(layout, r, QStringLiteral("t-ram")).first().toMap();
        QCOMPARE(m.value(QStringLiteral("filled")).toMap().value(QStringLiteral("y")).toDouble(), 197.0);
        chart.filledAt.reset();
        layout.blocks[0] = {QStringLiteral("Loaded"), chart};
        QVERIFY(!buildDetailBlocks(layout, r, QStringLiteral("t-ram")).first().toMap().contains(QStringLiteral("filled")));
    }

    void hoverNumbers()
    {
        const auto n = [](double v) { return hoverNumber(v); };
        QCOMPARE(n(12.35), QStringLiteral("12.35"));
        QCOMPARE(n(400), QStringLiteral("400"));
        QCOMPARE(n(1000.0 / 3), QStringLiteral("333.3"));
        QCOMPARE(n(1000.0 / 2.4271), QStringLiteral("412"));
        QCOMPARE(n(4812.3), QStringLiteral("4,812"));
        QCOMPARE(n(1048576), QStringLiteral("1,048,576"));
        QCOMPARE(n(0.8), QStringLiteral("0.8"));
        QCOMPARE(n(0.05), QStringLiteral("0.05"));
        QCOMPARE(n(0), QStringLiteral("0"));
        QCOMPARE(n(-2.5), QStringLiteral("-2.5"));
        QCOMPARE(n(9.9996), QStringLiteral("10"));
        QCOMPARE(n(NAN), QStringLiteral("—"));
        QCOMPARE(hoverValue(5836.8, QStringLiteral("KiB")), QStringLiteral("5.7 MiB"));
        QCOMPARE(hoverValue(24, QStringLiteral("KiB")), QStringLiteral("24 KiB"));
        QCOMPARE(hoverValue(1048576, QStringLiteral("KiB")), QStringLiteral("1 GiB"));
        QCOMPARE(hoverValue(98.25, QStringLiteral("%")), QStringLiteral("98.25%"));
        QCOMPARE(hoverValue(61.5, QStringLiteral("°C")), QStringLiteral("61.5 °C"));
        QCOMPARE(hoverValue(3, QString()), QStringLiteral("3"));
    }

    void lineChartFpsFromFrameTimes()
    {
        TestResult r;
        r.series = {{QStringLiteral("ft.vulkan.geometry"), QStringLiteral("Geometry"), QStringLiteral("Time"), QStringLiteral("s"),
                     QStringLiteral("ms"), {{0.05, 2.5}, {0.15, 0.0}, {0.25, 3.0}, {12.35, 2.4271}}}};
        LineChartBlock chart;
        chart.seriesKeys = {QStringLiteral("ft.vulkan.geometry")};
        chart.yValue = ChartValue::PerSecondFromMs;
        chart.yUnit = QStringLiteral("fps");
        const auto build = [&](const LineChartBlock &b) {
            DetailLayout layout;
            layout.blocks.append({QStringLiteral("G"), b});
            return buildDetailBlocks(layout, r, QStringLiteral("t-gpu")).first().toMap();
        };
        QVariantMap m = build(chart);
        const QVariantList pts = m.value(QStringLiteral("points")).toList();
        QCOMPARE(pts.size(), 3);
        QVERIFY(qFuzzyCompare(pts[0].toMap().value(QStringLiteral("y")).toDouble(), 400.0));
        QVERIFY(qFuzzyCompare(pts[1].toMap().value(QStringLiteral("y")).toDouble(), 1000.0 / 3));
        QVERIFY(qFuzzyCompare(pts[2].toMap().value(QStringLiteral("y")).toDouble(), 1000.0 / 2.4271));
        QCOMPARE(pts[0].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("0.05 s · 400 fps"));
        QCOMPARE(pts[1].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("0.25 s · 333.3 fps"));
        QCOMPARE(pts[2].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("12.35 s · 412 fps"));
        QCOMPARE(m.value(QStringLiteral("xUnit")).toString(), QStringLiteral("s"));
        chart.filledAt = 0.25;
        chart.markers = {12.35};
        m = build(chart);
        QVERIFY(qFuzzyCompare(m.value(QStringLiteral("filled")).toMap().value(QStringLiteral("y")).toDouble(), 1000.0 / 3));
        QVERIFY(qFuzzyCompare(m.value(QStringLiteral("markers")).toList().first().toMap().value(QStringLiteral("y")).toDouble(),
                              1000.0 / 2.4271));
        chart.seriesKeys = {QStringLiteral("nope")};
        QCOMPARE(build(chart).value(QStringLiteral("xUnit")).toString(), QString());
    }

    void lineChartAsStoredHasLabels()
    {
        TestResult r;
        r.series = {{QStringLiteral("c"), QStringLiteral("C"), {}, QStringLiteral("KiB"), QStringLiteral("ns"),
                     {{24, 0.8}, {5836.8, 9.6}, {1048576, 74.2}}}};
        LineChartBlock chart;
        chart.seriesKeys = {QStringLiteral("c")};
        chart.yUnit = QStringLiteral("ns");
        DetailLayout layout;
        layout.blocks.append({QStringLiteral("C"), chart});
        const QVariantList pts =
            buildDetailBlocks(layout, r, QStringLiteral("t-ram")).first().toMap().value(QStringLiteral("points")).toList();
        QCOMPARE(pts.size(), 3);
        QCOMPARE(pts[0].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("24 KiB · 0.8 ns"));
        QCOMPARE(pts[1].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("5.7 MiB · 9.6 ns"));
        QCOMPARE(pts[2].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("1 GiB · 74.2 ns"));
        QCOMPARE(pts[1].toMap().value(QStringLiteral("y")).toDouble(), 9.6);
        QCOMPARE(LineChartBlock{}.yValue, ChartValue::AsStored);
    }

    void tableSummaryIsExported()
    {
        TestResult r;
        TableBlock t;
        const auto summary = [&](const TableBlock &b) {
            DetailLayout layout;
            layout.blocks.append({QStringLiteral("T"), b});
            return buildDetailBlocks(layout, r, QStringLiteral("t")).first().toMap();
        };
        QVERIFY(summary(t).contains(QStringLiteral("summary")));
        QCOMPARE(summary(t).value(QStringLiteral("summary")).toString(), QString());
        t.summary = QStringLiteral("Vulkan 412 fps");
        QCOMPARE(summary(t).value(QStringLiteral("summary")).toString(), QStringLiteral("Vulkan 412 fps"));
    }

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

    // Zeb D2: a headline metric with a short Results-row label
    // (DetailLayout::headlineLabels): the RAM test's all-thread read.
    void headlineUsesTheShortLabel()
    {
        startRuntime();
        TestResult ram;
        ram.testId = QStringLiteral("ram");
        ram.status = Status::Ok;
        ram.metrics = {{QStringLiteral("latency.dram"), QStringLiteral("latency"), QStringLiteral("DRAM latency"), 75.8,
                        QStringLiteral("ns"), Direction::LowerIsBetter, true},
                       {QStringLiteral("bandwidth.read"), QStringLiteral("bandwidth"), QStringLiteral("Read bandwidth (all threads)"),
                        67.0, QStringLiteral("GB/s"), Direction::HigherIsBetter, true}};
        const RunRecord run = seed(RunStatus::Completed, QDateTime(QDate(2026, 10, 20), QTime(9, 0), QTimeZone::UTC), {ram});
        ResultsController &res = m_rt->results();
        QVERIFY(res.openRun(run.uuid.toString(QUuid::WithoutBraces)));
        QCOMPARE(res.rows()[0].toMap().value(QStringLiteral("raw")).toString(),
                 QStringLiteral("DRAM 75.8 ns · Read 67.0 GB/s"));
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
        // A test stopped with the orchestrator's default reason says "Stopped"
        // once (the score column), not twice; a reason with more in it stays.
        const RunRecord stoppedTests = seed(RunStatus::Stopped, QDateTime::currentDateTimeUtc().addSecs(2), {[] {
                                                TestResult r = TestResult::withStatus(Status::Cancelled, defaultStoppedReason());
                                                r.testId = QStringLiteral("cpu");
                                                return r;
                                            }(), [] {
                                                TestResult r = TestResult::withStatus(Status::Cancelled, QStringLiteral("Stopped during the write pass"));
                                                r.testId = QStringLiteral("storage");
                                                return r;
                                            }()});
        QVERIFY(res.openRun(stoppedTests.uuid.toString(QUuid::WithoutBraces)));
        QCOMPARE(res.rows()[0].toMap().value(QStringLiteral("score")).toString(), QStringLiteral("Stopped"));
        QCOMPARE(res.rows()[0].toMap().value(QStringLiteral("raw")).toString(), QString());
        QCOMPARE(res.rows()[1].toMap().value(QStringLiteral("score")).toString(), QStringLiteral("Stopped"));
        QCOMPARE(res.rows()[1].toMap().value(QStringLiteral("raw")).toString(), QStringLiteral("Stopped during the write pass"));

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
        // Stubs only (Phase 3 N6: the RAM test is real now; its Detail page
        // is tst_ram_test's). The stubs' chart and KPIs are Battery's (tst_stubs,
        // tst_results.qml).
        for (const QString &id : {QStringLiteral("storage"), QStringLiteral("ai")})
            rc.toggle(id);
        QSignalSpy finished(&rc, &RunController::runFinished);
        QVERIFY(rc.start());
        QVERIFY(finished.wait(60000));
        ResultsController &res = m_rt->results();
        // The finished run replaced the live view automatically.
        QCOMPARE(res.uuid(), finished.last().at(0).toString());
        QVERIFY(!res.live());
        QCOMPARE(res.rows().size(), 2);

        res.showDetail(QStringLiteral("ai"));
        QVERIFY(res.detailOpen());
        QCOMPARE(res.detailTitle(), QStringLiteral("AI inference"));
        QVERIFY(res.detailSubtitle().startsWith(QLatin1StringView("Score ")));
        QVERIFY(!block(res.detailBlocks(), QStringLiteral("table")).isEmpty());
        // Phase 4a M1: the exported table carries boldCells (empty when none are set).
        QVERIFY(block(res.detailBlocks(), QStringLiteral("table")).contains(QStringLiteral("boldCells")));
        QCOMPARE(res.detailColorToken(), QStringLiteral("t-ai"));
        res.showDetail(QStringLiteral("storage"));
        const auto grid = block(res.detailBlocks(), QStringLiteral("grid"));
        QCOMPARE(grid.value(QStringLiteral("rows")).toList().size(), 4);
        res.closeDetail();
        QVERIFY(!res.detailOpen());

        // D16: the log was saved with the run and reads back, filterable.
        auto *log = qobject_cast<LogFilterModel *>(res.logModel());
        QVERIFY(log);
        QVERIFY(log->rowCount() >= 10);
        const int all = log->rowCount();
        log->setTestFilter(QStringLiteral("ai"));
        QVERIFY(log->rowCount() > 0 && log->rowCount() < all);
        QVERIFY(log->visibleText().contains(QLatin1StringView("AI inference")));
        QVERIFY(!log->visibleText().contains(QLatin1StringView("Storage")));
        log->setTestFilter({});
        log->setMinLevel(QStringLiteral("warning"));
        QVERIFY(log->rowCount() < all);
        QCOMPARE(res.logTests().size(), 2);
    }

    void followsTheLiveRun()
    {
        tests::setStubTimeScale(0.3);
        startRuntime();
        RunController &rc = m_rt->run();
        rc.toggle(QStringLiteral("ai"));
        rc.toggle(QStringLiteral("storage"));
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

        // AP-5 #9: "View log" during a run keeps each line's own time (it
        // used to stamp every line with "now") and says it's a snapshot.
        QTest::qWait(2200);
        auto *shown = qobject_cast<QAbstractItemModel *>(res.logModel());
        auto *liveLog = qobject_cast<QAbstractItemModel *>(rc.log());
        QVERIFY(shown && liveLog);
        QVERIFY(shown->rowCount() >= 1);
        QCOMPARE(shown->data(shown->index(0, 0), LogModel::TimeRole), liveLog->data(liveLog->index(0, 0), LogModel::TimeRole));
        QVERIFY(res.live());  // the dialog's snapshot note keys off this
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
    // AP-5 re-check: Results follows the session's run when it finishes,
    // unless an older run was opened on purpose from History.
    void resultsFollowsTheSessionRunUnlessPinned()
    {
        tests::setStubTimeScale(0.02);
        startRuntime();
        const auto old = seed(RunStatus::Completed, QDateTime::currentDateTimeUtc().addDays(-3), {cpuResult(500)});
        ResultsController &res = m_rt->results();
        RunController &rc = m_rt->run();

        QVERIFY(res.openRun({}));  // an older run is showing, not pinned
        QSignalSpy finished(&rc, &RunController::runFinished);
        rc.toggle(QStringLiteral("ai"));
        QVERIFY(rc.start());
        QVERIFY(finished.wait(60000));
        QCOMPARE(res.uuid(), rc.runUuid());  // followed it

        // Opened on purpose: a finished run doesn't take over...
        QVERIFY(res.openFromHistory(old.uuid.toString(QUuid::WithoutBraces)));
        QVERIFY(res.pinned());
        QCOMPARE(res.uuid(), old.uuid.toString(QUuid::WithoutBraces));
        // ...but the next run un-pins it and is followed.
        finished.clear();
        rc.clearSelection();
        rc.toggle(QStringLiteral("ai"));
        QVERIFY(rc.start());
        QVERIFY(!res.pinned());
        QVERIFY(finished.wait(60000));
        QCOMPARE(res.uuid(), rc.runUuid());
    }

    // AP-5 #7: when the open run is gone, the detail page closes and the view
    // falls back to the newest run that's left.
    void goneRunClosesDetailAndFallsBack()
    {
        startRuntime();
        seed(RunStatus::Completed, QDateTime::currentDateTimeUtc(), {cpuResult(500)});
        ResultsController &res = m_rt->results();
        QVERIFY(res.openRun({}));
        res.showDetail(QStringLiteral("cpu"));
        QVERIFY(res.detailOpen());
        QCOMPARE(m_rt->storageController().clearHistory(), QString());
        QVERIFY(!res.detailOpen());
        QVERIFY(!res.valid());

        // A history switched to one with another run: that run shows.
        const QString other = m_dir->filePath(QStringLiteral("other"));
        QDir().mkpath(other);
        QUuid otherId;
        {
            const QString path = QDir(other).filePath(QStringLiteral("synbench.db"));
            QVERIFY(Database::migrate(path).ok());
            Database::ScopedConnection c(path, QStringLiteral("seed"));
            RunRecord r;
            r.uuid = otherId = QUuid::createUuid();
            r.status = RunStatus::Completed;
            r.startedAt = QDateTime::currentDateTimeUtc();
            r.tests = {cpuResult(900)};
            QVERIFY(RunRepository(c.db()).saveRun(r));
        }
        seed(RunStatus::Completed, QDateTime::currentDateTimeUtc(), {cpuResult(100)});
        QVERIFY(res.openRun({}));
        res.showDetail(QStringLiteral("cpu"));
        QCOMPARE(m_rt->storageController().openHistoryIn(other), QString());
        QVERIFY(!res.detailOpen());
        QCOMPARE(res.uuid(), otherId.toString(QUuid::WithoutBraces));
    }

    // N2 (Dave): Stopped and Interrupted are partial everywhere, by one rule.
    void stoppedAndInterruptedArePartialEverywhere()
    {
        startRuntime();
        const QDateTime t0 = QDateTime(QDate(2026, 10, 1), QTime(12, 0), QTimeZone::UTC);
        const auto stopped = seed(RunStatus::Stopped, t0, {cpuResult(1000)});
        const auto interrupted = seed(RunStatus::Interrupted, t0.addDays(1), {cpuResult(1000)});
        const auto recovered = seed(RunStatus::Recovered, t0.addDays(2), {cpuResult(1000)});
        const auto done = seed(RunStatus::Completed, t0.addDays(3), {cpuResult(1000)});
        QVERIFY(isPartialRun(RunStatus::Running));
        QVERIFY(isPartialRun(RunStatus::Stopped));
        QVERIFY(isPartialRun(RunStatus::Interrupted));
        QVERIFY(!isPartialRun(RunStatus::Recovered));
        QVERIFY(!isPartialRun(RunStatus::Completed));

        HistoryModel &h = m_rt->history();
        h.reload();  // newest first: done, recovered, interrupted, stopped
        QCOMPARE(h.data(h.index(0), HistoryModel::PartialRole).toBool(), false);
        QCOMPARE(h.data(h.index(1), HistoryModel::PartialRole).toBool(), false);
        QCOMPARE(h.data(h.index(2), HistoryModel::PartialRole).toBool(), true);
        QCOMPARE(h.data(h.index(3), HistoryModel::PartialRole).toBool(), true);

        ResultsController &res = m_rt->results();
        for (const auto &[run, partial, note] :
             {std::tuple{stopped, true, "Partial: the run was stopped"}, std::tuple{interrupted, true, "Partial: the run was interrupted"},
              std::tuple{recovered, false, ""}, std::tuple{done, false, ""}}) {
            QVERIFY(res.openRun(run.uuid.toString(QUuid::WithoutBraces)));
            QCOMPARE(res.overallPartial(), partial);
            if (partial)
                QCOMPARE(res.overallNote(), QString::fromLatin1(note));
        }

        // Compare: a partial run is marked, and its overall gets no change %,
        // also when the partial run is the baseline.
        CompareController &c = m_rt->compare();
        QVERIFY(c.setRuns({done.uuid.toString(QUuid::WithoutBraces), interrupted.uuid.toString(QUuid::WithoutBraces)}));
        auto overallCells = [&] {
            for (const QVariant &v : c.scoreRows())
                if (v.toMap().value(QStringLiteral("overall")).toBool())
                    return v.toMap().value(QStringLiteral("cells")).toList();
            return QVariantList{};
        };
        QVERIFY(overallCells().at(1).toMap().value(QStringLiteral("partial")).toBool());
        QCOMPARE(overallCells().at(1).toMap().value(QStringLiteral("changeText")).toString(), QString());
        c.setBaseline(interrupted.uuid.toString(QUuid::WithoutBraces));
        QCOMPARE(overallCells().at(0).toMap().value(QStringLiteral("changeText")).toString(), QString());
    }

    // --- History and Compare (task 16) ---

    void historyListsNewestFirstAndPages()
    {
        startRuntime();
        const QDateTime t0 = QDateTime(QDate(2026, 10, 1), QTime(12, 0), QTimeZone::UTC);
        seed(RunStatus::Completed, t0, {cpuResult(1000)});
        seed(RunStatus::Stopped, t0.addDays(1), {cpuResult(1100)});
        seed(RunStatus::Completed, t0.addDays(2), {batteryResult(402, QStringLiteral("mixed"))}, RunKind::Exclusive);
        HistoryModel &h = m_rt->history();
        h.reload();
        QCOMPARE(h.totalCount(), 3);
        QCOMPARE(h.rowCount(), 3);
        // newest first: the Battery run
        QCOMPARE(h.data(h.index(0), HistoryModel::TestsRole).toString(), QStringLiteral("Battery (Mixed)"));
        QCOMPARE(h.data(h.index(0), HistoryModel::OverallRole).toString(), QStringLiteral("6 h 42 m"));
        QCOMPARE(h.data(h.index(1), HistoryModel::StatusRole).toString(), QStringLiteral("Stopped"));
        QCOMPARE(h.data(h.index(1), HistoryModel::PartialRole).toBool(), true);  // N2
        QCOMPARE(h.data(h.index(2), HistoryModel::StatusRole).toString(), QString());
        QCOMPARE(h.data(h.index(2), HistoryModel::PartialRole).toBool(), false);
        QVERIFY(!h.canFetchMore({}));

        // Ticking: two or more to compare.
        QVERIFY(!h.canCompare());
        QCOMPARE(h.compareText(), QStringLiteral("Pick 2 or more"));
        h.toggleChecked(h.data(h.index(1), HistoryModel::UuidRole).toString());
        h.toggleChecked(h.data(h.index(2), HistoryModel::UuidRole).toString());
        QVERIFY(h.canCompare());
        QCOMPARE(h.compareText(), QStringLiteral("Compare 2 runs"));
        QCOMPARE(h.checkedUuids().size(), 2);
        QCOMPARE(h.checkedUuids().first(), h.data(h.index(1), HistoryModel::UuidRole).toString());  // newest first
        h.clearChecked();
        QCOMPARE(h.checkedCount(), 0);
    }

    void historyPagesAt200()
    {
        startRuntime();
        const QDateTime t0 = QDateTime(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::UTC);
        for (int i = 0; i < 205; ++i)
            seed(RunStatus::Completed, t0.addSecs(i * 60), {cpuResult(900 + i)});
        HistoryModel &h = m_rt->history();
        h.reload();
        QCOMPARE(h.totalCount(), 205);
        QCOMPARE(h.rowCount(), 200);
        QVERIFY(h.canFetchMore({}));
        h.fetchMore({});
        QCOMPARE(h.rowCount(), 205);
        QVERIFY(!h.canFetchMore({}));
    }

    // AP-5 #13: a run saved between two pages shifts the offsets; the boundary
    // row must not show twice, and paging must still end.
    void historyPagingDoesNotDuplicateTheBoundaryRow()
    {
        startRuntime();
        const QDateTime t0 = QDateTime(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::UTC);
        for (int i = 0; i < 205; ++i)
            seed(RunStatus::Completed, t0.addSecs(i * 60), {cpuResult(900 + i)});
        HistoryModel &h = m_rt->history();
        h.reload();
        QCOMPARE(h.rowCount(), 200);
        seed(RunStatus::Completed, t0.addDays(5), {cpuResult(1)});  // newer than everything: shifts the offsets by one
        while (h.canFetchMore({}))
            h.fetchMore({});
        QSet<QString> ids;
        for (int i = 0; i < h.rowCount(); ++i)
            ids.insert(h.data(h.index(i), HistoryModel::UuidRole).toString());
        QCOMPARE(ids.size(), h.rowCount());  // no duplicates
        QCOMPARE(h.rowCount(), 205);         // the new run isn't in this view until a reload
    }

    // AP-5 #13: odd percentages and NaN never reach llround undefined.
    void changeTextHandlesHugeAndNonFinite()
    {
        QCOMPARE(CompareController::changeText(1e300, false), QStringLiteral("+99999%"));
        QCOMPARE(CompareController::changeText(-1e300, false), QStringLiteral("-99999%"));
        QCOMPARE(CompareController::changeText(std::nan(""), false), QString());
        QCOMPARE(CompareController::changeText(std::numeric_limits<double>::infinity(), true), QString());
    }

    void historyDropsTicksOfClearedRuns()
    {
        startRuntime();
        const auto a = seed(RunStatus::Completed, QDateTime::currentDateTimeUtc(), {cpuResult(1)});
        HistoryModel &h = m_rt->history();
        h.reload();
        h.toggleChecked(a.uuid.toString(QUuid::WithoutBraces));
        QCOMPARE(h.checkedCount(), 1);
        QCOMPARE(m_rt->storageController().clearHistory(), QString());
        QCOMPARE(h.totalCount(), 0);
        QCOMPARE(h.checkedCount(), 0);
    }

    // Zeb #5: two runs on the same day are told apart by the short header lines.
    void compareHeadersTellSameDayRunsApart()
    {
        startRuntime();
        const QDateTime morning = QDateTime(QDate(2026, 10, 13), QTime(9, 5), QTimeZone::UTC);
        const auto a = seed(RunStatus::Completed, morning, {cpuResult(1000)});
        const auto b = seed(RunStatus::Completed, morning.addSecs(3600 * 4), {cpuResult(1100)});
        CompareController &c = m_rt->compare();
        QVERIFY(c.setRuns({a.uuid.toString(QUuid::WithoutBraces), b.uuid.toString(QUuid::WithoutBraces)}));
        const QVariantMap first = c.runs().at(0).toMap(), second = c.runs().at(1).toMap();
        QCOMPARE(first.value(QStringLiteral("day")), second.value(QStringLiteral("day")));      // same day
        QVERIFY(first.value(QStringLiteral("timeAndHost")) != second.value(QStringLiteral("timeAndHost")));  // different line 2
        QVERIFY(first.value(QStringLiteral("timeAndHost")).toString().endsWith(QStringLiteral(" · Knowhere")));  // the host, not the whole label
        QVERIFY(first.value(QStringLiteral("date")).toString().contains(QStringLiteral("2026")));  // the full text stays for the tooltip
    }

    void compareTwoRunsWithBaselineSwitch()
    {
        startRuntime();
        const QDateTime t0 = QDateTime(QDate(2026, 10, 1), QTime(12, 0), QTimeZone::UTC);
        const auto older = seed(RunStatus::Completed, t0, {cpuResult(1000)});
        const auto newer = seed(RunStatus::Completed, t0.addDays(1), {cpuResult(1360)});
        CompareController &c = m_rt->compare();
        QVERIFY(!c.setRuns({older.uuid.toString(QUuid::WithoutBraces)}));  // needs two
        QVERIFY(c.setRuns({older.uuid.toString(QUuid::WithoutBraces), newer.uuid.toString(QUuid::WithoutBraces)}));
        QCOMPARE(c.runs().size(), 2);
        QCOMPARE(c.baselineUuid(), newer.uuid.toString(QUuid::WithoutBraces));  // D5: newest

        auto row = [&](const QString &id) {
            for (const QVariant &v : c.scoreRows())
                if (v.toMap().value(QStringLiteral("testId")).toString() == id)
                    return v.toMap();
            return QVariantMap{};
        };
        auto cell = [](const QVariantMap &r, int i) { return r.value(QStringLiteral("cells")).toList().at(i).toMap(); };
        // Columns newest first: [newer(baseline), older]. older vs newer = -26%.
        QCOMPARE(cell(row(QStringLiteral("cpu")), 0).value(QStringLiteral("changeText")).toString(), QString());
        QCOMPARE(cell(row(QStringLiteral("cpu")), 1).value(QStringLiteral("changeText")).toString(), QStringLiteral("-26%"));
        QCOMPARE(cell(row(QStringLiteral("cpu")), 1).value(QStringLiteral("direction")).toString(), QStringLiteral("bad"));

        c.setBaseline(older.uuid.toString(QUuid::WithoutBraces));
        QCOMPARE(c.baselineUuid(), older.uuid.toString(QUuid::WithoutBraces));
        QCOMPARE(cell(row(QStringLiteral("cpu")), 0).value(QStringLiteral("changeText")).toString(), QStringLiteral("+36%"));
        QCOMPARE(cell(row(QStringLiteral("cpu")), 0).value(QStringLiteral("direction")).toString(), QStringLiteral("good"));
        QVERIFY(c.runs().at(1).toMap().value(QStringLiteral("baseline")).toBool());

        // Raw metrics side by side.
        QCOMPARE(c.rawTestId(), QStringLiteral("cpu"));
        QVERIFY(!c.rawRows().isEmpty());
        QCOMPARE(c.rawRows().first().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Single-thread mark"));
    }

    void compareFourRunsMissingTestsAndScoringChange()
    {
        startRuntime();
        const QDateTime t0 = QDateTime(QDate(2026, 10, 1), QTime(12, 0), QTimeZone::UTC);
        QStringList ids;
        TestResult v2 = cpuResult(1500);
        v2.formulaVersion = 2;  // D6
        const QVector<QVector<TestResult>> sets = {{cpuResult(1000)}, {cpuResult(1100)}, {batteryResult(300, QStringLiteral("mixed"))}, {v2}};
        for (int i = 0; i < 4; ++i)
            ids << seed(i == 1 ? RunStatus::Stopped : RunStatus::Completed, t0.addDays(i), sets[i],
                        i == 2 ? RunKind::Exclusive : RunKind::Standard).uuid.toString(QUuid::WithoutBraces);
        CompareController &c = m_rt->compare();
        QVERIFY(c.setRuns(ids));
        QCOMPARE(c.runs().size(), 4);
        QCOMPARE(c.baselineUuid(), ids[3]);
        QVariantMap cpu, overall;
        for (const QVariant &v : c.scoreRows()) {
            if (v.toMap().value(QStringLiteral("testId")).toString() == QLatin1StringView("cpu"))
                cpu = v.toMap();
            if (v.toMap().value(QStringLiteral("overall")).toBool())
                overall = v.toMap();
        }
        QCOMPARE(cpu.value(QStringLiteral("cells")).toList().size(), 4);
        QVERIFY(cpu.value(QStringLiteral("scoringChanged")).toBool());  // D6
        // The Battery-only run has no CPU result: a dash, not a number.
        QCOMPARE(cpu.value(QStringLiteral("cells")).toList().at(1).toMap().value(QStringLiteral("text")).toString(), QStringLiteral("—"));
        // N2: the Stopped run (index 2, newest first) is marked partial.
        QVERIFY(overall.value(QStringLiteral("cells")).toList().at(2).toMap().value(QStringLiteral("partial")).toBool());
        QVERIFY(!overall.value(QStringLiteral("cells")).toList().at(0).toMap().value(QStringLiteral("partial")).toBool());
        // Dave's N2 decision: no change % on a partial overall (no scary red delta).
        QCOMPARE(overall.value(QStringLiteral("cells")).toList().at(2).toMap().value(QStringLiteral("changeText")).toString(), QString());
        QCOMPARE(overall.value(QStringLiteral("cells")).toList().at(2).toMap().value(QStringLiteral("direction")).toString(), QStringLiteral("none"));
        // D6 on the raw rows: against another formula version the % is still
        // shown, with the "scoring changed" marker; a text row gets neither.
        c.setBaseline(ids[0]);  // v1; the formula-2 run is column 0
        QCOMPARE(c.rawTestId(), QStringLiteral("cpu"));
        auto rawRow = [&](const QString &label) {
            for (const QVariant &v : c.rawRows())
                if (v.toMap().value(QStringLiteral("label")).toString() == label)
                    return v.toMap().value(QStringLiteral("cells")).toList();
            return QVariantList{};
        };
        const QVariantList mark = rawRow(QStringLiteral("Single-thread mark"));
        QCOMPARE(mark.size(), 4);
        QVERIFY(mark.at(0).toMap().value(QStringLiteral("changeText")).toString().endsWith(QLatin1Char('%')));
        QVERIFY(mark.at(0).toMap().value(QStringLiteral("scoringChanged")).toBool());
        QVERIFY(mark.at(2).toMap().value(QStringLiteral("changeText")).toString().endsWith(QLatin1Char('%')));  // same versions
        QVERIFY(!mark.at(2).toMap().value(QStringLiteral("scoringChanged")).toBool());
        const QVariantList simd = rawRow(QStringLiteral("SIMD path"));
        QCOMPARE(simd.size(), 4);
        QCOMPARE(simd.at(0).toMap().value(QStringLiteral("changeText")).toString(), QString());
        QVERIFY(!simd.at(0).toMap().value(QStringLiteral("scoringChanged")).toBool());
        QCOMPARE(CompareController::changeText(52.4, true), QStringLiteral("+52% (worse)"));
        QCOMPARE(CompareController::changeText(-12.2, true), QStringLiteral("-12%"));
        QCOMPARE(CompareController::changeText(0.2, false), QStringLiteral("0%"));
    }
};

QTEST_MAIN(TestResults)
#include "tst_results.moc"
