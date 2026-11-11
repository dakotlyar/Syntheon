// SPDX-License-Identifier: GPL-3.0-or-later
// The Storage test's Detail page (Phase 4b design §13, task 10/12): the
// CrystalDiskMark-style grid (rows = the four seq/rnd patterns, columns =
// Read/Write, with IOPS as the secondary metric on the random rows), a latency
// table, and a mixed table. Built only from the existing GridBlock/TableBlock
// (design N10); no new renderer. Task 12 adds the KPI block, the engine in the
// subtitle and the notes.
#include "tests/storage/ids.h"
#include "tests/storage/storage_test.h"
#include "tests/common/stub_test.h"

#include "core/units.h"

namespace sb::tests {

namespace {

const char *kDash = "—";

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

} // namespace

DetailLayout StorageTest::detail(const TestResult &r) const
{
    DetailLayout d;
    const QString drive = r.options.value(kDriveKey).toString();
    d.subtitle = scoreSubtitle(r, {drive, QStringLiteral("cache bypassed")});
    d.headlineMetrics = {QStringLiteral("seq1m_q8t1.read"), QStringLiteral("rnd4k_q32t1.read_iops"),
                         QStringLiteral("mixed.throughput"), QStringLiteral("latency.p50")};

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
        d.blocks.append({{}, grid});

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

    return d;
}

} // namespace sb::tests
