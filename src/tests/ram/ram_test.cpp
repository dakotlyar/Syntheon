// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/ram_test.h"

#include <QJsonArray>
#include <QJsonObject>

namespace sb::tests {

namespace {

StubSpec makeSpec()
{
    StubSpec s;
    TestInfo &i = s.info;
    i.id = QStringLiteral("ram");
    i.name = QStringLiteral("RAM");
    i.summary = QStringLiteral("Latency curve, timings, bandwidth, loaded latency");
    i.colorToken = QStringLiteral("t-ram");
    i.subTests = {{QStringLiteral("bandwidth"), QStringLiteral("Bandwidth"), true, true},
                  {QStringLiteral("latency"), QStringLiteral("Latency sweep"), true, true},
                  {QStringLiteral("loaded"), QStringLiteral("Loaded latency"), true, true}};
    s.estimate = std::chrono::seconds(18);
    s.targetScore = 612;
    s.estimateFactor = [](const OptionValues &o) { return o.value(QStringLiteral("duration"), 10.0).toDouble() / 10.0; };

    const auto metric = [&](const char *key, const char *sub, const char *label, const char *unit, double base,
                            Direction dir, double share, bool headline = false) {
        s.metrics.append({QString::fromLatin1(key), QString::fromLatin1(sub), QString::fromLatin1(label),
                          QString::fromLatin1(unit), base, dir, headline, share, {}});
    };
    metric("bandwidth.read", "bandwidth", "Read bandwidth", "GB/s", 78.4, Direction::HigherIsBetter, 0.35, true);
    metric("bandwidth.write", "bandwidth", "Write bandwidth", "GB/s", 71.0, Direction::HigherIsBetter, 0.20);
    metric("bandwidth.copy", "bandwidth", "Copy bandwidth", "GB/s", 69.8, Direction::HigherIsBetter, 0.15);
    metric("latency.dram", "latency", "DRAM latency", "ns", 74.2, Direction::LowerIsBetter, 0, true);
    metric("loaded.latency", "loaded", "Loaded latency", "ns", 128, Direction::LowerIsBetter, 0, true);
    metric("loaded.load", "loaded", "Load during loaded latency", "GB/s", 72.0, Direction::HigherIsBetter, 0.30);

    s.options = [](const ProbeContext &ctx, const Availability &) {
        OptionSchema o;
        OptionDef size;
        size.key = QStringLiteral("buffer");
        size.label = QStringLiteral("Test buffer size");
        size.kind = OptionKind::SingleSelect;
        size.items = {{QStringLiteral("256m"), QStringLiteral("256 MiB")},
                      {QStringLiteral("1g"), QStringLiteral("1 GiB")},
                      {QStringLiteral("4g"), QStringLiteral("4 GiB")}};
        size.defaultValue = QStringLiteral("1g");
        o.options.append(size);
        OptionDef duration;
        duration.key = QStringLiteral("duration");
        duration.label = QStringLiteral("Duration per sub-test");
        duration.kind = OptionKind::Number;
        duration.minimum = 5;
        duration.maximum = 60;
        duration.step = 5;
        duration.unit = QStringLiteral("s");
        duration.defaultValue = 10.0;
        o.options.append(duration);
        OptionDef mem;
        mem.key = QStringLiteral("detected");
        mem.label = QStringLiteral("Memory");
        mem.kind = OptionKind::Info;
        const double gib = snapshotNumber(ctx.system(), "memory", "total_bytes", 0) / (1024.0 * 1024 * 1024);
        mem.infoValue = gib > 0 ? QStringLiteral("%1 GB detected").arg(gib, 0, 'f', 1) : QStringLiteral("Detecting…");
        o.options.append(mem);
        return o;
    };

    s.extra = [](TestResult &r, const OptionValues &o, std::mt19937 &rng, const SystemSnapshot &sys) {
        double speed = 5600;
        int channels = 2;
        const QJsonArray modules = sys.data.value(QStringLiteral("memory")).toObject().value(QStringLiteral("modules")).toArray();
        if (!modules.isEmpty()) {
            const QJsonObject m = modules.first().toObject();
            speed = m.value(QStringLiteral("configured_speed_mts")).toDouble(m.value(QStringLiteral("speed_mts")).toDouble(speed));
            channels = sys.data.value(QStringLiteral("memory")).toObject().value(QStringLiteral("channels")).toInt(channels);
        }
        r.metrics.append(numberMetric(QStringLiteral("speed"), QStringLiteral("Configured speed"), speed, QStringLiteral("MT/s"),
                                      Direction::Neutral, true));
        r.metrics.append(numberMetric(QStringLiteral("channels"), QStringLiteral("Channels"), channels, QString()));
        r.metrics.append(textMetric(QStringLiteral("timings"), QStringLiteral("CL-tRCD-tRP-tRAS"), QStringLiteral("40-40-40-77")));
        if (optionContains(o, QString(kSubTestsOptionKey), QStringLiteral("latency"))) {
            // Latency by buffer size: L1 ~1 ns, L2 ~3, L3 ~10, DRAM ~74 ns.
            Series curve{QStringLiteral("latency_curve"), QStringLiteral("Latency by buffer size"), QStringLiteral("Buffer size"),
                         QStringLiteral("KiB"), QStringLiteral("ns"), {}};
            const double sizes[] = {4, 8, 16, 32, 64, 128, 256, 512, 1024, 4096, 16384, 32768, 65536, 131072, 262144,
                                    524288, 1048576};
            for (const double kib : sizes) {
                const double base = kib <= 32 ? 0.9 : kib <= 512 ? 3.0 : kib <= 16384 ? 10.5 : kib <= 32768 ? 22
                                    : kib <= 65536 ? 52 : kib <= 131072 ? 66 : kib <= 262144 ? 71 : 74.2;
                curve.points.append({kib, vary(rng, base)});
            }
            r.series.append(curve);
        }
    };

    s.detail = [](const TestResult &r) {
        DetailLayout d;
        d.subtitle = scoreSubtitle(r, {metricText(r, QStringLiteral("speed")), metricText(r, QStringLiteral("channels")) + QStringLiteral(" channels")});
        d.headlineMetrics = {QStringLiteral("bandwidth.read"), QStringLiteral("latency.dram")};
        KpisBlock kpis;
        const auto add = [&](const QString &label, const QString &key) {
            if (r.metric(key))
                kpis.items.append({label, DetailCell::metric(key), {}});
        };
        add(QStringLiteral("DRAM latency"), QStringLiteral("latency.dram"));
        add(QStringLiteral("Loaded latency"), QStringLiteral("loaded.latency"));
        add(QStringLiteral("Read bandwidth"), QStringLiteral("bandwidth.read"));
        add(QStringLiteral("Configured"), QStringLiteral("speed"));
        d.blocks.append({{}, kpis});
        if (!r.series.isEmpty()) {
            LineChartBlock chart;
            chart.seriesKeys = {QStringLiteral("latency_curve")};
            chart.xAxis.scale = ChartAxis::Scale::Log2;
            chart.xAxis.tickLabels = {QStringLiteral("4K"), QStringLiteral("32K"), QStringLiteral("1M"), QStringLiteral("16M"),
                                      QStringLiteral("128M"), QStringLiteral("1G")};
            chart.yUnit = QStringLiteral("ns");
            chart.annotations = {{32, QStringLiteral("L1")}, {512, QStringLiteral("L2")}, {16384, QStringLiteral("L3")},
                                 {524288, QStringLiteral("DRAM")}};
            d.blocks.append({QStringLiteral("Latency by buffer size"), chart});
        }
        TableBlock bw;
        bw.columns = {QStringLiteral("Bandwidth"), QStringLiteral("GB/s")};
        for (const auto &row : {std::pair{"Read", "bandwidth.read"}, {"Write", "bandwidth.write"}, {"Copy", "bandwidth.copy"}}) {
            if (r.metric(QString::fromLatin1(row.second)))
                bw.rows.append({DetailCell::literal(QString::fromLatin1(row.first)), DetailCell::metric(QString::fromLatin1(row.second))});
        }
        if (!bw.rows.isEmpty())
            d.blocks.append({{}, bw});
        TableBlock timings;
        timings.columns = {QStringLiteral("Timings"), QStringLiteral("Value")};
        timings.rows = {{DetailCell::literal(QStringLiteral("Speed")), DetailCell::metric(QStringLiteral("speed"))},
                        {DetailCell::literal(QStringLiteral("CL-tRCD-tRP-tRAS")), DetailCell::metric(QStringLiteral("timings"))},
                        {DetailCell::literal(QStringLiteral("Channels")), DetailCell::metric(QStringLiteral("channels"))}};
        d.blocks.append({{}, timings});
        return d;
    };
    return s;
}

} // namespace

RamTest::RamTest() : StubTest(makeSpec()) {}

} // namespace sb::tests
