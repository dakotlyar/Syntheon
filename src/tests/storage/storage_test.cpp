// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/storage/storage_test.h"

#include <QStorageInfo>

namespace sb::tests {

namespace {

struct Row {
    const char *sub, *label, *small;
    double read, write, readIops, writeIops;  // 0 IOPS = sequential
};
constexpr Row kRows[] = {
    {"seq1m_q8t1", "Seq 1M", "Q8T1", 7012, 6284, 0, 0},
    {"seq1m_q1t1", "Seq 1M", "Q1T1", 3920, 3410, 0, 0},
    {"rnd4k_q32t1", "Random 4K", "Q32T1", 742, 590, 181000, 144000},
    {"rnd4k_q1t1", "Random 4K", "Q1T1", 82, 260, 20000, 63000},
};

StubSpec makeSpec()
{
    StubSpec s;
    TestInfo &i = s.info;
    i.id = QStringLiteral("storage");
    i.name = QStringLiteral("Storage");
    i.summary = QStringLiteral("Sequential and random read/write, latency, mixed");
    i.colorToken = QStringLiteral("t-sto");
    i.subTests = {{QStringLiteral("seq1m_q8t1"), QStringLiteral("Seq 1M Q8T1"), true, true},
                  {QStringLiteral("seq1m_q1t1"), QStringLiteral("Seq 1M Q1T1"), true, true},
                  {QStringLiteral("rnd4k_q32t1"), QStringLiteral("Random 4K Q32T1"), true, true},
                  {QStringLiteral("rnd4k_q1t1"), QStringLiteral("Random 4K Q1T1"), true, true},
                  {QStringLiteral("latency"), QStringLiteral("Latency (4K Q1T1)"), true, true},
                  {QStringLiteral("mixed"), QStringLiteral("Mixed 70/30 random"), true, true}};
    s.estimate = std::chrono::seconds(28);
    s.targetScore = 938;
    s.estimateFactor = [](const OptionValues &o) {
        const QString size = o.value(QStringLiteral("file_size"), QStringLiteral("1g")).toString();
        return size == QLatin1StringView("256m") ? 0.6 : size == QLatin1StringView("4g") ? 1.8 : 1.0;
    };

    const double shares[][2] = {{0.20, 0.15}, {0.10, 0.08}, {0.12, 0.10}, {0.10, 0.10}};
    int n = 0;
    for (const Row &row : kRows) {
        const QString sub = QString::fromLatin1(row.sub);
        const QString name = QStringLiteral("%1 %2").arg(QString::fromLatin1(row.label), QString::fromLatin1(row.small));
        s.metrics.append({sub + QStringLiteral(".read"), sub, name + QStringLiteral(" read"), QStringLiteral("MB/s"), row.read,
                          Direction::HigherIsBetter, n == 0 || n == 3, shares[n][0], {}});
        s.metrics.append({sub + QStringLiteral(".write"), sub, name + QStringLiteral(" write"), QStringLiteral("MB/s"), row.write,
                          Direction::HigherIsBetter, false, shares[n][1], {}});
        if (row.readIops > 0) {
            s.metrics.append({sub + QStringLiteral(".read_iops"), sub, name + QStringLiteral(" read IOPS"), QStringLiteral("IOPS"),
                              row.readIops, Direction::HigherIsBetter, false, 0, {}});
            s.metrics.append({sub + QStringLiteral(".write_iops"), sub, name + QStringLiteral(" write IOPS"), QStringLiteral("IOPS"),
                              row.writeIops, Direction::HigherIsBetter, false, 0, {}});
        }
        ++n;
    }
    for (const auto &lat : {std::tuple{"p50", "p50 latency", 68.0}, {"p99", "p99 latency", 210.0}, {"p999", "p99.9 latency", 1840.0}}) {
        s.metrics.append({QStringLiteral("latency.") + QString::fromLatin1(std::get<0>(lat)), QStringLiteral("latency"),
                          QString::fromLatin1(std::get<1>(lat)), QStringLiteral("µs"), std::get<2>(lat), Direction::LowerIsBetter, false, 0, {}});
    }
    s.metrics.append({QStringLiteral("mixed.throughput"), QStringLiteral("mixed"), QStringLiteral("Mixed throughput"), QStringLiteral("MB/s"),
                      1120, Direction::HigherIsBetter, false, 0.03, {}});
    s.metrics.append({QStringLiteral("mixed.iops"), QStringLiteral("mixed"), QStringLiteral("Mixed IOPS"), QStringLiteral("IOPS"), 273000,
                      Direction::HigherIsBetter, false, 0.02, {}});

    s.options = [](const ProbeContext &, const Availability &) {
        OptionSchema o;
        OptionDef drive;
        drive.key = QStringLiteral("drive");
        drive.label = QStringLiteral("Drive");
        drive.kind = OptionKind::DynamicSelect;
        QString first;
        for (const QStorageInfo &v : QStorageInfo::mountedVolumes()) {
            if (!v.isValid() || !v.isReady() || v.isReadOnly() || v.bytesTotal() < (qint64(1) << 30))
                continue;
            const QString device = QString::fromLocal8Bit(v.device());
#ifdef Q_OS_UNIX
            if (!device.startsWith(QLatin1StringView("/dev/")) || device.startsWith(QLatin1StringView("/dev/loop")))
                continue;
#endif
            OptionItem item;
            item.value = v.rootPath();
            item.label = v.name().isEmpty() ? v.rootPath() : QStringLiteral("%1 (%2)").arg(v.rootPath(), v.name());
            item.detail = QStringLiteral("%1 · %2 GB free · %3")
                              .arg(QString::fromLatin1(v.fileSystemType()))
                              .arg(double(v.bytesAvailable()) / 1e9, 0, 'f', 0)
                              .arg(QStringLiteral("cache bypassed"));
            if (first.isEmpty())
                first = item.value;
            drive.items.append(item);
        }
        drive.defaultValue = first;
        o.options.append(drive);
        OptionDef size;
        size.key = QStringLiteral("file_size");
        size.label = QStringLiteral("Test file size");
        size.kind = OptionKind::SingleSelect;
        size.presentation = SelectPresentation::Segmented;
        size.items = {{QStringLiteral("256m"), QStringLiteral("256 MiB")},
                      {QStringLiteral("1g"), QStringLiteral("1 GiB")},
                      {QStringLiteral("4g"), QStringLiteral("4 GiB")}};
        size.defaultValue = QStringLiteral("1g");
        o.options.append(size);
        OptionDef folder;
        folder.key = QStringLiteral("target_folder");
        folder.label = QStringLiteral("Test folder (optional)");
        folder.kind = OptionKind::Path;
        folder.defaultValue = QString();
        folder.help = QStringLiteral("Leave empty to use a folder on the selected drive.");
        o.options.append(folder);
        return o;
    };

    s.detail = [](const TestResult &r) {
        DetailLayout d;
        const QString drive = r.options.value(QStringLiteral("drive")).toString();
        d.subtitle = scoreSubtitle(r, {drive, QStringLiteral("cache bypassed")});
        d.headlineMetrics = {QStringLiteral("seq1m_q8t1.read"), QStringLiteral("rnd4k_q1t1.read")};
        GridBlock grid;
        grid.columnLabels = {QStringLiteral("Read"), QStringLiteral("Write")};
        for (const Row &row : kRows) {
            const QString sub = QString::fromLatin1(row.sub);
            if (!r.metric(sub + QStringLiteral(".read")))
                continue;
            grid.rowLabels.append(QString::fromLatin1(row.label));
            grid.rowSmallText.append(QString::fromLatin1(row.small));
            grid.cells.append({{sub + QStringLiteral(".read"), row.readIops > 0 ? sub + QStringLiteral(".read_iops") : QString()},
                               {sub + QStringLiteral(".write"), row.writeIops > 0 ? sub + QStringLiteral(".write_iops") : QString()}});
        }
        if (!grid.rowLabels.isEmpty())
            d.blocks.append({{}, grid});
        if (r.metric(QStringLiteral("latency.p50"))) {
            TableBlock lat;
            lat.columns = {QStringLiteral("Latency (4K Q1T1 read)"), QStringLiteral("µs")};
            lat.rows = {{DetailCell::literal(QStringLiteral("p50")), DetailCell::metric(QStringLiteral("latency.p50"))},
                        {DetailCell::literal(QStringLiteral("p99")), DetailCell::metric(QStringLiteral("latency.p99"))},
                        {DetailCell::literal(QStringLiteral("p99.9")), DetailCell::metric(QStringLiteral("latency.p999"))}};
            d.blocks.append({{}, lat});
        }
        if (r.metric(QStringLiteral("mixed.iops"))) {
            TableBlock mixed;
            mixed.columns = {QStringLiteral("Mixed 70/30 random"), QStringLiteral("Result")};
            mixed.rows = {{DetailCell::literal(QStringLiteral("Throughput")), DetailCell::metric(QStringLiteral("mixed.throughput"))},
                          {DetailCell::literal(QStringLiteral("IOPS")), DetailCell::metric(QStringLiteral("mixed.iops"))}};
            d.blocks.append({{}, mixed});
        }
        return d;
    };
    return s;
}

} // namespace

StorageTest::StorageTest() : StubTest(makeSpec()) {}

} // namespace sb::tests
