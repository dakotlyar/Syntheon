// SPDX-License-Identifier: GPL-3.0-or-later
// The Storage test's Detail page (Phase 4b design §13, task 12): the
// CrystalDiskMark-style grid (rows = the four seq/rnd patterns, columns =
// Read/Write, with IOPS as the secondary metric on the random rows), a latency
// table, and a mixed table, plus a KPI block (score + the four headline
// numbers), the engine in the subtitle and the cache-bypass / no-verify notes.
// Built only from the existing GridBlock/TableBlock/KpisBlock/NoteBlock
// (design N10); no new renderer.
#include "tests/storage/ids.h"
#include "tests/storage/storage_test.h"
#include "tests/common/stub_test.h"

#include "core/units.h"

namespace sb::tests {

namespace {

// The four grid rows (the mockup's shape): pattern, row label, small text.
struct GridRow {
    sb::storage::Pattern pattern;
    const char *label;
    const char *small;
};
constexpr GridRow kGridRows[] = {
    {sb::storage::Pattern::Seq1mQ8T1, "Seq 1M", "Q8T1"},
    {sb::storage::Pattern::Seq1mQ1T1, "Seq 1M", "Q1T1"},
    {sb::storage::Pattern::Rnd4kQ32T1, "Random 4K", "Q32T1"},
    {sb::storage::Pattern::Rnd4kQ1T1, "Random 4K", "Q1T1"},
};

bool hasIops(sb::storage::Pattern p)
{
    return p == sb::storage::Pattern::Rnd4kQ32T1 || p == sb::storage::Pattern::Rnd4kQ1T1;
}

const Metric *metric(const TestResult &r, const char *key)
{
    return r.metric(QLatin1StringView(key));
}

QString text(const TestResult &r, const char *key)
{
    const Metric *m = metric(r, key);
    if (!m || m->isNumeric())
        return {};
    return std::get<QString>(m->value);
}

} // namespace

DetailLayout StorageTest::detail(const TestResult &r) const
{
    DetailLayout d;
    const QString drive = r.options.value(kDriveKey).toString();
    const QString engine = text(r, "model.io");
    const QString kind = text(r, "target.kind");
    const QString wearGb = metric(r, "wear.write_gb") ? QString::number(std::llround(std::get<double>(metric(r, "wear.write_gb")->value))) : QString();

    // Subtitle: "Score 1,000 · <drive> · cache bypassed · <engine>" (§13).
    QStringList parts;
    if (drive.isEmpty())
        parts << QStringLiteral("no drive");
    else
        parts << drive;
    parts << QStringLiteral("cache bypassed");
    if (!engine.isEmpty())
        parts << engine;
    d.subtitle = scoreSubtitle(r, parts);

    // The four headline numbers (§13; the score is the first KPI).
    d.headlineMetrics = {QStringLiteral("seq1m_q8t1.read"), QStringLiteral("rnd4k_q32t1.read_iops"),
                         QStringLiteral("mixed.throughput"), QStringLiteral("latency.p50")};
    d.headlineLabels.insert(QStringLiteral("seq1m_q8t1.read"), QStringLiteral("Seq read"));
    d.headlineLabels.insert(QStringLiteral("rnd4k_q32t1.read_iops"), QStringLiteral("4K IOPS"));
    d.headlineLabels.insert(QStringLiteral("mixed.throughput"), QStringLiteral("Mixed"));

    // KPIs: the score, then the four headline numbers in plain language (§13).
    KpisBlock kpis;
    if (r.score)
        kpis.items.append({QStringLiteral("Storage score"),
                           DetailCell::literal(Units::format(*r.score, QStringLiteral("points"), false)),
                           QStringLiteral("points")});
    auto kpi = [&](const char *label, const char *key, const QString &unit) {
        const Metric *m = metric(r, key);
        if (!m)
            return;
        const double v = std::get<double>(m->value);
        kpis.items.append({QLatin1StringView(label), DetailCell::literal(Units::format(v, unit, false)), unit});
    };
    kpi("Seq 1M Q8T1 read", "seq1m_q8t1.read", QStringLiteral("MB/s"));
    kpi("Random 4K Q32T1 read", "rnd4k_q32t1.read_iops", QStringLiteral("IOPS"));
    kpi("Mixed throughput", "mixed.throughput", QStringLiteral("MB/s"));
    kpi("p50 latency", "latency.p50", QStringLiteral("µs"));
    if (!kpis.items.isEmpty())
        d.blocks.append({{}, kpis});

    // The CrystalDiskMark-style grid (N10): rows = patterns, columns = Read/Write.
    GridBlock grid;
    grid.columnLabels = {QStringLiteral("Read"), QStringLiteral("Write")};
    for (const GridRow &row : kGridRows) {
        const QString read = sb::storage::key(row.pattern, sb::storage::Field::Read);
        if (!r.metric(read))
            continue;
        grid.rowLabels.append(QLatin1StringView(row.label));
        grid.rowSmallText.append(QLatin1StringView(row.small));
        grid.cells.append({{read, hasIops(row.pattern) ? sb::storage::key(row.pattern, sb::storage::Field::ReadIops) : QString()},
                           {sb::storage::key(row.pattern, sb::storage::Field::Write),
                            hasIops(row.pattern) ? sb::storage::key(row.pattern, sb::storage::Field::WriteIops) : QString()}});
    }
    if (!grid.rowLabels.isEmpty())
        d.blocks.append({QStringLiteral("Sequential and random"), grid});

    if (r.metric(QStringLiteral("latency.p50"))) {
        TableBlock lat;
        lat.columns = {QStringLiteral("Latency (4K Q1T1 read)"), QStringLiteral("µs")};
        lat.rows = {{DetailCell::literal(QStringLiteral("p50")), DetailCell::metric(QStringLiteral("latency.p50"))},
                    {DetailCell::literal(QStringLiteral("p99")), DetailCell::metric(QStringLiteral("latency.p99"))},
                    {DetailCell::literal(QStringLiteral("p99.9")), DetailCell::metric(QStringLiteral("latency.p999"))}};
        d.blocks.append({{}, lat});
    }

    if (r.metric(QStringLiteral("mixed.throughput"))) {
        TableBlock mixed;
        mixed.columns = {QStringLiteral("Mixed 70/30 random"), QStringLiteral("Result")};
        mixed.rows = {{DetailCell::literal(QStringLiteral("Throughput")), DetailCell::metric(QStringLiteral("mixed.throughput"))},
                      {DetailCell::literal(QStringLiteral("IOPS")), DetailCell::metric(QStringLiteral("mixed.iops"))}};
        d.blocks.append({{}, mixed});
    }

    // The notes (§13): cache bypassed, no data verification, the write
    // footprint, and the HDD caveat.
    QString note = QStringLiteral("Cache bypassed (O_DIRECT / unbuffered I/O); data not verified");
    if (!wearGb.isEmpty())
        note += QStringLiteral("; writes about %1 GB").arg(wearGb);
    note += QStringLiteral(".");
    d.blocks.append({{}, NoteBlock{note, Tone::Good}});
    if (kind == QLatin1StringView("HDD"))
        d.blocks.append({{}, NoteBlock{QStringLiteral("The sequential patterns dominate on a spinning disk; for 4 KiB "
                                                       "random I/O, an SSD is the meaningful comparison."),
                                       Tone::Neutral}});

    return d;
}

} // namespace sb::tests
