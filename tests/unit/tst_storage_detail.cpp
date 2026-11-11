// SPDX-License-Identifier: GPL-3.0-or-later
// The Storage Detail page (Phase 4b design §13 + D8 mockup): the subtitle, the
// KPI block, the CrystalDiskMark-style grid, the latency and mixed tables (with
// their sub-scores and titles), the Sub-tests weight + sub-score table, the
// weight line, and the notes, from a hand-built result. Mirrors tst_ram_test.
#include "tests/storage/storage_test.h"
#include "tests/storage/ids.h"

#include <QTest>

using namespace sb;
using namespace sb::tests;

namespace {

// A completed result with every metric at its anchor, all six sub-scores, and
// the neutral text.
TestResult completedResult()
{
    TestResult r;
    r.status = Status::Ok;
    r.score = 1000.0;
    r.options = {{QStringLiteral("drive"), QStringLiteral("/data")}, {QStringLiteral("file_size"), QStringLiteral("4g")}};
    const auto num = [&](const char *key, const char *sub, const char *label, double value, const char *unit) {
        r.metrics.append({QString::fromLatin1(key), QLatin1StringView(sub), QLatin1StringView(label), value,
                          QLatin1StringView(unit), Direction::HigherIsBetter, false});
    };
    num("seq1m_q8t1.read", "seq1m_q8t1", "Seq 1M Q8T1 read", 7012, "MB/s");
    num("seq1m_q8t1.write", "seq1m_q8t1", "Seq 1M Q8T1 write", 6284, "MB/s");
    num("seq1m_q1t1.read", "seq1m_q1t1", "Seq 1M Q1T1 read", 3920, "MB/s");
    num("seq1m_q1t1.write", "seq1m_q1t1", "Seq 1M Q1T1 write", 3410, "MB/s");
    num("rnd4k_q32t1.read", "rnd4k_q32t1", "Random 4K Q32T1 read", 742, "MB/s");
    num("rnd4k_q32t1.read_iops", "rnd4k_q32t1", "Random 4K Q32T1 read IOPS", 181000, "IOPS");
    num("rnd4k_q32t1.write", "rnd4k_q32t1", "Random 4K Q32T1 write", 590, "MB/s");
    num("rnd4k_q32t1.write_iops", "rnd4k_q32t1", "Random 4K Q32T1 write IOPS", 144000, "IOPS");
    num("rnd4k_q1t1.read", "rnd4k_q1t1", "Random 4K Q1T1 read", 82, "MB/s");
    num("rnd4k_q1t1.read_iops", "rnd4k_q1t1", "Random 4K Q1T1 read IOPS", 20000, "IOPS");
    num("rnd4k_q1t1.write", "rnd4k_q1t1", "Random 4K Q1T1 write", 260, "MB/s");
    num("rnd4k_q1t1.write_iops", "rnd4k_q1t1", "Random 4K Q1T1 write IOPS", 63000, "IOPS");
    r.metrics.append({QStringLiteral("latency.p50"), QStringLiteral("latency"), QStringLiteral("p50 latency"), 68.0,
                      QStringLiteral("µs"), Direction::LowerIsBetter, false});
    r.metrics.append({QStringLiteral("latency.p99"), QStringLiteral("latency"), QStringLiteral("p99 latency"), 210.0,
                      QStringLiteral("µs"), Direction::LowerIsBetter, false});
    r.metrics.append({QStringLiteral("latency.p999"), QStringLiteral("latency"), QStringLiteral("p99.9 latency"), 1840.0,
                      QStringLiteral("µs"), Direction::LowerIsBetter, false});
    num("mixed.throughput", "mixed", "Mixed throughput", 1100, "MB/s");
    num("mixed.iops", "mixed", "Mixed IOPS", 273000, "IOPS");
    r.metrics.append({QStringLiteral("model.io"), {}, QStringLiteral("I/O engine"), QStringLiteral("io_uring"), {},
                      Direction::Neutral, false});
    r.metrics.append({QStringLiteral("target.drive"), {}, QStringLiteral("Target drive"), QStringLiteral("/data"), {},
                      Direction::Neutral, false});
    r.metrics.append({QStringLiteral("target.kind"), {}, QStringLiteral("Drive kind"), QStringLiteral("SSD"), {},
                      Direction::Neutral, false});
    r.metrics.append({QStringLiteral("wear.write_gb"), {}, QStringLiteral("Estimated writes"), 8.0, QStringLiteral("GB"),
                      Direction::Neutral, false});
    // All six sub-results, each Ok with a sub-score of 1000.
    r.subResults = {{QStringLiteral("seq1m_q8t1"), Status::Ok, {}, 1000.0},
                    {QStringLiteral("seq1m_q1t1"), Status::Ok, {}, 1000.0},
                    {QStringLiteral("rnd4k_q32t1"), Status::Ok, {}, 1000.0},
                    {QStringLiteral("rnd4k_q1t1"), Status::Ok, {}, 1000.0},
                    {QStringLiteral("latency"), Status::Ok, {}, 1000.0},
                    {QStringLiteral("mixed"), Status::Ok, {}, 1000.0}};
    return r;
}

QStringList gridCellKeys(const GridBlock &g, int row, int col)
{
    return {g.cells[row][col].metricKey, g.cells[row][col].secondaryMetricKey};
}

QStringList tableRows(const TableBlock &t)
{
    QStringList out;
    for (const auto &row : t.rows) {
        QString r;
        for (const DetailCell &c : row)
            r += (r.isEmpty() ? QString() : QStringLiteral(" | ")) + (c.metricKey.isEmpty() ? c.text : c.metricKey);
        out << r;
    }
    return out;
}

const TableBlock *tableTitled(const DetailLayout &d, const char *title)
{
    for (const DetailBlock &b : d.blocks)
        if (b.title == QLatin1StringView(title))
            if (const auto *tb = std::get_if<TableBlock>(&b.content))
                return tb;
    return nullptr;
}

QStringList noteTexts(const DetailLayout &d)
{
    QStringList out;
    for (const DetailBlock &b : d.blocks)
        if (const auto *n = std::get_if<NoteBlock>(&b.content))
            out << n->text;
    return out;
}

} // namespace

class TestStorageDetail : public QObject
{
    Q_OBJECT

private slots:
    void completedLayout()
    {
        StorageTest t;
        const DetailLayout d = t.detail(completedResult());
        QCOMPARE(d.subtitle, QStringLiteral("Score 1,000 · /data · cache bypassed · io_uring"));
        QCOMPARE(d.headlineMetrics,
                 (QStringList{QStringLiteral("seq1m_q8t1.read"), QStringLiteral("rnd4k_q32t1.read_iops"),
                              QStringLiteral("mixed.throughput"), QStringLiteral("latency.p50")}));
        QCOMPARE(d.headlineLabels.value(QStringLiteral("rnd4k_q32t1.read_iops")), QStringLiteral("4K read"));

        // KPI block: score + four headline numbers.
        const auto &kpis = std::get<KpisBlock>(d.blocks.first().content);
        QCOMPARE(kpis.items.size(), 5);
        QCOMPARE(kpis.items[0].label, QStringLiteral("Storage score"));
        QCOMPARE(kpis.items[4].label, QStringLiteral("p50 latency"));

        // The weight line note.
        const QStringList notes = noteTexts(d);
        QVERIFY(notes.contains(QStringLiteral("Seq 1M Q8T1 12.5% · Seq 1M Q1T1 12.5% · Rnd 4K Q32T1 22.5% · "
                                              "Rnd 4K Q1T1 22.5% · Mixed 15% · Latency 15%")));

        // The grid: 4 rows x 2 columns, Read/Write, IOPS secondary on random.
        const GridBlock *grid = nullptr;
        for (const DetailBlock &b : d.blocks)
            if (const auto *g = std::get_if<GridBlock>(&b.content))
                grid = g;
        QVERIFY(grid);
        QCOMPARE(grid->columnLabels, (QStringList{QStringLiteral("Read"), QStringLiteral("Write")}));
        QCOMPARE(grid->rowLabels.size(), 4);
        QCOMPARE(gridCellKeys(*grid, 2, 0), (QStringList{QStringLiteral("rnd4k_q32t1.read"), QStringLiteral("rnd4k_q32t1.read_iops")}));

        // The latency table (titled) with its sub-score.
        const TableBlock *lat = tableTitled(d, "Latency (4K Q1T1 read)");
        QVERIFY(lat);
        QCOMPARE(tableRows(*lat), (QStringList{QStringLiteral("p50 | latency.p50"), QStringLiteral("p99 | latency.p99"),
                                               QStringLiteral("p99.9 | latency.p999"), QStringLiteral("Score | 1,000")}));
        QVERIFY(!lat->summary.isEmpty());

        // The mixed table (titled) with its score.
        const TableBlock *mixed = tableTitled(d, "Mixed 70/30 random");
        QVERIFY(mixed);
        QCOMPARE(tableRows(*mixed), (QStringList{QStringLiteral("Throughput | mixed.throughput"),
                                                 QStringLiteral("IOPS | mixed.iops"), QStringLiteral("Score | 1,000")}));

        // The Sub-tests weight + sub-score table.
        const TableBlock *groups = tableTitled(d, "Sub-tests");
        QVERIFY(groups);
        QCOMPARE(groups->columns, (QStringList{QStringLiteral("Sub-test"), QStringLiteral("Weight"), QStringLiteral("Sub-score")}));
        QCOMPARE(groups->rows.size(), 6);
        QCOMPARE(groups->rows[0][0].text, QStringLiteral("Seq 1M Q8T1"));
        QCOMPARE(groups->rows[0][1].text, QStringLiteral("12.5%"));
        QCOMPARE(groups->rows[0][2].text, QStringLiteral("1,000"));
        QCOMPARE(groups->summary,
                 QStringLiteral("Seq 1M Q8T1 1,000 · Seq 1M Q1T1 1,000 · Rnd 4K Q32T1 1,000 · "
                                "Rnd 4K Q1T1 1,000 · Mixed 1,000 · Latency 1,000"));

        // The cache-bypass note (not the HDD caveat on an SSD).
        QVERIFY(std::any_of(notes.begin(), notes.end(), [](const QString &n) {
            return n.startsWith(QLatin1StringView("Cache bypassed (O_DIRECT / unbuffered I/O); data not verified"));
        }));
        QVERIFY(std::any_of(notes.begin(), notes.end(), [](const QString &n) { return n.contains(QLatin1StringView("writes about 8 GB")); }));
        QVERIFY(!std::any_of(notes.begin(), notes.end(), [](const QString &n) { return n.contains(QLatin1StringView("spinning disk")); }));
    }

    void hddNote()
    {
        StorageTest t;
        TestResult r = completedResult();
        for (Metric &m : r.metrics)
            if (m.key == QLatin1StringView("target.kind"))
                m.value = QStringLiteral("HDD");
        const QStringList hddNotes = noteTexts(t.detail(r));
        QVERIFY(std::any_of(hddNotes.begin(), hddNotes.end(), [](const QString &n) { return n.contains(QLatin1StringView("spinning disk")); }));
        // The HDD caveat surfaces as a Warn note, not buried in a table.
        const DetailLayout d = t.detail(r);
        for (const DetailBlock &b : d.blocks)
            if (const auto *nb = std::get_if<NoteBlock>(&b.content); nb && nb->text.contains(QLatin1StringView("spinning disk")))
                QCOMPARE(nb->tone, Tone::Warn);
    }

    void noScoreSubtitle()
    {
        StorageTest t;
        TestResult r = completedResult();
        r.score.reset();
        const DetailLayout d = t.detail(r);
        QVERIFY(!d.subtitle.contains(QStringLiteral("Score ")));
        QVERIFY(d.subtitle.contains(QStringLiteral("/data · cache bypassed · io_uring")));
    }
};

QTEST_GUILESS_MAIN(TestStorageDetail)
#include "tst_storage_detail.moc"
