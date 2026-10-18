// SPDX-License-Identifier: GPL-3.0-or-later
// The assembled CPU test (Phase 2 design task 14): a whole run at the
// developer time scale through a fake RunContext, its metrics, sub-results,
// series and log; Stop in each phase within 250 ms; a failing group makes the
// test Partial; and the TestInfo / options / estimate.
#include "core/units.h"
#include "privileged/privileged_ops.h"
#include "tests/common/stub_clock.h"
#include "tests/cpu/cpu_test.h"
#include "tests/cpu/crypto_client.h"
#include "tests/cpu/engine/platform_threads.h"
#include "tests/cpu/formula_v1.h"
#include "core/detail.h"

#include <QTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <mutex>

using namespace sb;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

class FakeRunContext : public RunContext
{
public:
    void progress(double fraction, const QString &stage) override
    {
        std::lock_guard lock(m_mutex);
        if (!m_progress.isEmpty() && fraction + 1e-9 < m_progress.last().first)
            m_backwards = true;
        m_progress.append({fraction, stage});
        if (!m_cancelOn.isEmpty() && stage.startsWith(m_cancelOn) && !m_cancel) {
            m_cancelAt = Clock::now();
            m_cancel = true;
        }
    }
    void log(LogLevel level, const QString &message) override
    {
        std::lock_guard lock(m_mutex);
        m_logs.append({level, message});
    }
    bool cancelled() const override { return m_cancel.load(); }
    void subTestBegin(const QString &id) override
    {
        std::lock_guard lock(m_mutex);
        m_subTests.append(id + QStringLiteral(":begin"));
    }
    void subTestEnd(const QString &id, Status status) override
    {
        std::lock_guard lock(m_mutex);
        m_subTests.append(id + QStringLiteral(":") + toKey(status));
    }
    void checkpoint(const QCborMap &) override {}
    QString scratchDir() override { return {}; }
    const SystemSnapshot &system() const override { return m_system; }
    PrivilegedOps &privileged() override { return m_priv; }

    // Raises the cancel the first time a progress stage starts with `stage`.
    void cancelOn(const QString &stage) { m_cancelOn = stage; }
    Clock::time_point cancelAt() const { return m_cancelAt; }
    bool logged(const QString &part) const
    {
        std::lock_guard lock(m_mutex);
        for (const auto &l : m_logs) {
            if (l.second.contains(part))
                return true;
        }
        return false;
    }
    QString allLogs() const
    {
        std::lock_guard lock(m_mutex);
        QStringList out;
        for (const auto &l : m_logs)
            out << toKey(l.first) + QStringLiteral(": ") + l.second;
        return out.join(QLatin1Char('\n'));
    }
    QList<QPair<double, QString>> progressCalls() const
    {
        std::lock_guard lock(m_mutex);
        return m_progress;
    }
    QStringList subTests() const
    {
        std::lock_guard lock(m_mutex);
        return m_subTests;
    }
    bool wentBackwards() const { return m_backwards; }

private:
    mutable std::mutex m_mutex;
    QList<QPair<LogLevel, QString>> m_logs;
    QList<QPair<double, QString>> m_progress;
    QStringList m_subTests;
    std::atomic<bool> m_cancel{false};
    Clock::time_point m_cancelAt{};
    QString m_cancelOn;
    bool m_backwards = false;
    SystemSnapshot m_system;
    UnavailablePrivilegedOps m_priv;
};

// The mockup's completed run (all numbers illustrative, as drawn).
TestResult mockupResult()
{
    TestResult r;
    r.testId = QStringLiteral("cpu");
    r.status = Status::Ok;
    r.score = 875;
    const auto num = [&](const char *key, const char *sub, double v, const char *unit) {
        r.metrics.append({QLatin1StringView(key), QLatin1StringView(sub), QString(), v, QLatin1StringView(unit),
                          Direction::HigherIsBetter, false});
    };
    const auto txt = [&](const char *key, const QString &v) {
        r.metrics.append({QLatin1StringView(key), {}, QString(), v, {}, Direction::Neutral, false});
    };
    num("single.int", "", 6710, "MOps/s");
    num("single.fp", "", 4030, "MFLOPS");
    num("single.compress", "", 211, "MB/s");
    num("single.mark", "", 921, "points");
    txt("single.cpu", QStringLiteral("CPU 0 and 5 (the quietest top-ranked core, chosen per repetition)"));
    txt("single.cores", QStringLiteral("integer 0,0,5,0,0 · FP 5,0,0,0,5 · compression 0,0,0,0,0"));
    num("int.rate", "int", 148600, "MOps/s");
    num("fp.rate", "fp", 91800, "MFLOPS");
    num("fp.primes", "fp", 2140, "MOps/s");
    txt("simd.path", QStringLiteral("AVX-512 (512-bit)"));
    num("simd.rate", "simd", 412000, "MOps/s");
    const double c[] = {640, 4750, 11300, 47}, dd[] = {6380, 19100, 63800, 1590}, ratio[] = {3.12, 3.36, 2.08, 3.94};
    const char *keys[] = {"zlib", "zstd", "lz4", "xz"};
    for (int i = 0; i < 4; ++i) {
        r.metrics.append({QStringLiteral("compress.%1.c").arg(QLatin1StringView(keys[i])), QStringLiteral("compress"), QString(), c[i],
                          QStringLiteral("MB/s"), Direction::HigherIsBetter, false});
        r.metrics.append({QStringLiteral("compress.%1.d").arg(QLatin1StringView(keys[i])), QStringLiteral("compress"), QString(), dd[i],
                          QStringLiteral("MB/s"), Direction::HigherIsBetter, false});
        r.metrics.append({QStringLiteral("compress.%1.ratio").arg(QLatin1StringView(keys[i])), QStringLiteral("compress"), QString(),
                          ratio[i], QString(), Direction::Neutral, false});
    }
    txt("crypto.path", QStringLiteral("AES-NI + VAES, SHA-NI"));
    num("crypto.aes_gcm", "crypto", 96400, "MB/s");
    num("crypto.sha256", "crypto", 24300, "MB/s");
    num("crypto.aes_gcm.sw", "crypto", 3120, "MB/s");
    num("crypto.sha256.sw", "crypto", 6150, "MB/s");
    num("sort.u64", "sort", 362, "MOps/s");
    num("sort.str", "sort", 118, "MOps/s");
    num("physics.nbody", "physics", 2890, "MOps/s");
    num("threads", "", 24, "");
    r.metrics.append({QStringLiteral("temp.max"), {}, QStringLiteral("Peak temperature (k10temp Tctl)"), 95.0, QStringLiteral("°C"),
                      Direction::Neutral, false});
    txt("throttle", QStringLiteral("Detected"));
    txt("throttle.detail", QStringLiteral("xz compression: clock 4.6 → 3.9 GHz (−15%)"));
    txt("throttle.note", QStringLiteral("Throttling detected in xz compression: clock speed fell from 4.6 to 3.9 GHz (−15%) at "
                                        "95 °C. The score is not adjusted."));
    const double scores[] = {862, 891, 803, 905, 944, 872, 910};
    const char *ids[] = {"int", "fp", "simd", "compress", "crypto", "sort", "physics"};
    for (int i = 0; i < 7; ++i)
        r.subResults.append({QLatin1StringView(ids[i]), Status::Ok, {}, scores[i]});
    Series clock{QStringLiteral("telemetry.clock"), QStringLiteral("Clock (CPUs running the test)"), QStringLiteral("Time"),
                 QStringLiteral("s"), QStringLiteral("MHz"), {}};
    Series temp{QStringLiteral("telemetry.temp"), QStringLiteral("Temperature (k10temp Tctl)"), QStringLiteral("Time"),
                QStringLiteral("s"), QStringLiteral("°C"), {}};
    for (int s = 0; s <= 270; ++s) {
        clock.points.append({double(s), 4600.0});
        temp.points.append({double(s), s == 150 ? 95.0 : 70.0});
    }
    Series windows{QStringLiteral("telemetry.windows"), QString(), QString(), QStringLiteral("s"), QString(), {}};
    const std::pair<int, double> starts[] = {{1, 3}, {2, 20}, {3, 37}, {10, 59}, {11, 81}, {12, 103}, {13, 115}, {20, 137},
                                             {21, 146}, {26, 182}, {27, 192}, {30, 198}, {31, 206}, {32, 214}, {33, 219},
                                             {40, 224}, {41, 236}, {50, 248}};
    for (const auto &[code, x] : starts)
        windows.points.append({x, double(code)});
    r.series = {clock, temp, windows};
    return r;
}

const char *const kScoredKeys[] = {
    "int.rate",         "fp.rate",          "fp.primes",       "simd.rate",        "compress.zlib.c", "compress.zlib.d",
    "compress.zstd.c",  "compress.zstd.d",  "compress.lz4.c",  "compress.lz4.d",   "compress.xz.c",   "compress.xz.d",
    "crypto.aes_gcm",   "crypto.sha256",    "sort.u64",        "sort.str",         "physics.nbody",   "single.int",
    "single.fp",        "single.compress",
};

} // namespace

class TestCpuTest : public QObject
{
    Q_OBJECT

private slots:
    void init() { tests::setStubTimeScale(0.01); }
    void cleanup()
    {
        tests::setStubTimeScale(0);
        cpu::CryptoClient::setWorkerForTests({});
    }

    void identity()
    {
        tests::CpuTest t;
        QCOMPARE(t.info().id, QStringLiteral("cpu"));
        QCOMPARE(t.info().testVersion, 0);  // until calibration (task 18)
        QStringList subs;
        for (const SubTestInfo &s : t.info().subTests) {
            subs << s.id;
            QVERIFY(!s.userSelectable);  // D14: all or nothing
        }
        QCOMPARE(subs, (QStringList{QStringLiteral("int"), QStringLiteral("fp"), QStringLiteral("simd"),
                                    QStringLiteral("compress"), QStringLiteral("crypto"), QStringLiteral("sort"),
                                    QStringLiteral("physics")}));
        tests::setStubTimeScale(1);
        QCOMPARE(t.estimate({}), std::chrono::seconds(270));
        QCOMPARE(t.timeLimit({}), std::chrono::seconds(600));  // 2 x estimate + 60 s
    }

    // A whole run at 1% of real time: every group and the single-thread mark.
    void wholeRun()
    {
        tests::CpuTest t;
        FakeRunContext ctx;
        const auto start = Clock::now();
        const TestResult r = t.run(ctx, {});
        qInfo("whole run at 1%%: %.1f s", std::chrono::duration<double>(Clock::now() - start).count());
        QVERIFY2(r.status == Status::Ok, qPrintable(r.statusReason + QStringLiteral("\n") + ctx.allLogs()));
        for (const char *key : kScoredKeys) {
            const Metric *m = r.metric(QLatin1StringView(key));
            QVERIFY2(m && m->isNumeric() && std::get<double>(m->value) > 0, key);
            QVERIFY2(Units::isCanonical(m->unit), key);
            QCOMPARE(m->direction, Direction::HigherIsBetter);
        }
        for (const char *key : {"compress.zlib.ratio", "compress.xz.ratio", "crypto.aes_gcm.sw", "crypto.sha256.sw",
                                "background.load", "stability.spread", "threads"})
            QVERIFY2(r.metric(QLatin1StringView(key)), key);
        for (const char *key : {"simd.path", "crypto.path", "crypto.sw_path", "single.cpu", "throttle", "power.source"}) {
            const Metric *m = r.metric(QLatin1StringView(key));
            QVERIFY2(m && !m->isNumeric() && !std::get<QString>(m->value).isEmpty(), key);
        }
        QCOMPARE(std::get<double>(r.metric(QStringLiteral("threads"))->value), double(cpu::logicalCpuCount()));
        // The groups, in order, all Ok.
        QStringList ids;
        for (const SubResult &s : r.subResults) {
            ids << s.id;
            QCOMPARE(s.status, Status::Ok);
        }
        QCOMPARE(ids.size(), 7);
        QCOMPARE(ctx.subTests().size(), 14);
        // The window marks, and telemetry where this machine has it.
        bool marks = false;
        for (const Series &s : r.series)
            marks = marks || s.key == QLatin1StringView("telemetry.windows");
        QVERIFY(marks);
        // Progress only goes forward and ends at 1.
        QVERIFY(!ctx.wentBackwards());
        QCOMPARE(ctx.progressCalls().last().first, 1.0);
        QVERIFY(ctx.logged(QStringLiteral("Starting CPU:")));
        QVERIFY(ctx.logged(QStringLiteral("SIMD: ")));
        QVERIFY(ctx.logged(QStringLiteral("Encryption: hardware")));
        QVERIFY(ctx.logged(QStringLiteral("single.int on CPU")) || ctx.logged(QStringLiteral("unpinned")));
        // Task 15: the single-thread mark (a headline metric) and the score.
        const Metric *mark = r.metric(QStringLiteral("single.mark"));
        QVERIFY(mark && mark->headline && std::get<double>(mark->value) > 0);
        QCOMPARE(mark->unit, QStringLiteral("points"));
        QVERIFY(ctx.logged(QStringLiteral("Single-thread mark ")));
        const ScoreBreakdown b = t.score(r);
        QVERIFY(b.total && *b.total > 0);
        QCOMPARE(b.subScores.size(), 7);
    }

    // formula v1 (§7): at the anchors everything is 1,000; the weights act
    // as elasticities; a missing group means no total but keeps the others.
    void formula()
    {
        TestResult r;
        for (const GeometricGroup &g : cpu::formula_v1::groups()) {
            for (const GeometricMetric &m : g.metrics)
                r.metrics.append({m.metricKey, g.id, QString(), m.anchor, QString(), Direction::HigherIsBetter, false});
        }
        ScoreBreakdown b = cpu::formula_v1::score(r);
        QCOMPARE(b.total, std::optional<double>(1000));
        for (const double v : b.subScores)
            QCOMPARE(v, 1000.0);
        // The group weights sum to 1, and each group's metric weights too.
        double total = 0;
        for (const GeometricGroup &g : cpu::formula_v1::groups()) {
            total += g.weight;
            double inner = 0;
            for (const GeometricMetric &m : g.metrics)
                inner += m.weight;
            QVERIFY(qAbs(inner - 1) < 1e-12);
        }
        QVERIFY(qAbs(total - 1) < 1e-12);
        // SIMD twice as fast: 2^0.15 = +11%.
        for (Metric &m : r.metrics) {
            if (m.key == QLatin1StringView("simd.rate"))
                m.value = std::get<double>(m.value) * 2;
        }
        QCOMPARE(cpu::formula_v1::score(r).total, std::optional<double>(1110));
        // No encryption: no total; the rest keep their sub-scores.
        r.metrics.erase(std::remove_if(r.metrics.begin(), r.metrics.end(),
                                       [](const Metric &m) { return m.key.startsWith(QLatin1StringView("crypto.")); }),
                        r.metrics.end());
        b = cpu::formula_v1::score(r);
        QVERIFY(!b.total);
        QCOMPARE(b.subScores.size(), 6);
        QCOMPARE(b.subScores.value(QStringLiteral("simd")), 2000.0);
        // The single-thread mark: 5:5:2.
        using namespace cpu::formula_v1;
        QCOMPARE(singleThreadMark(kAnchorSingleInt, kAnchorSingleFp, kAnchorSingleCompress), std::optional<double>(1000));
        QCOMPARE(singleThreadMark(2 * kAnchorSingleInt, kAnchorSingleFp, kAnchorSingleCompress),
                 std::optional<double>(std::round(1000 * std::pow(2.0, 5.0 / 12.0))));
        QCOMPARE(singleThreadMark(kAnchorSingleInt, kAnchorSingleFp, 2 * kAnchorSingleCompress),
                 std::optional<double>(std::round(1000 * std::pow(2.0, 2.0 / 12.0))));
        QVERIFY(!singleThreadMark(0, kAnchorSingleFp, kAnchorSingleCompress));
    }

    // The dev telemetry-injection hook: a forced clock fall in one
    // measurement raises the throttling flag; the score doesn't change
    // because of it (it comes from the rates).
    void injectedThrottle()
    {
        qputenv("SYNBENCH_CPU_FAKE_THROTTLE", "xz compress");
        tests::CpuTest t;
        FakeRunContext ctx;
        const TestResult r = t.run(ctx, {});
        qunsetenv("SYNBENCH_CPU_FAKE_THROTTLE");
        QCOMPARE(r.status, Status::Ok);
        QCOMPARE(std::get<QString>(r.metric(QStringLiteral("throttle"))->value), QStringLiteral("Detected"));
        const QString detail = std::get<QString>(r.metric(QStringLiteral("throttle.detail"))->value);
        QVERIFY2(detail.startsWith(QStringLiteral("xz compression: clock ")), qPrintable(detail));
        QVERIFY(ctx.logged(QStringLiteral("xz compress: clock fell")));  // the log names the stage (mockup)
        QVERIFY(ctx.logged(QStringLiteral("Throttling detected in xz compression")));
        QVERIFY(ctx.logged(QStringLiteral("at 95 °C")));
        QVERIFY(t.score(r).total);
    }

    // Stop in each phase: the run returns within 250 ms as Cancelled, with
    // the groups that finished.
    void stopInEachPhase_data()
    {
        QTest::addColumn<QString>("stage");
        for (const char *stage : {"Preparing", "Preparing inputs", "Single-thread integer", "Primes", "zlib prepare",
                                  "xz compress", "AES-256-GCM", "Physics"})
            QTest::newRow(stage) << QString::fromLatin1(stage);
    }
    void stopInEachPhase()
    {
        QFETCH(QString, stage);
        tests::setStubTimeScale(0.02);  // windows long enough to stop inside
        tests::CpuTest t;
        FakeRunContext ctx;
        ctx.cancelOn(stage);
        const TestResult r = t.run(ctx, {});
        const auto took = Clock::now() - ctx.cancelAt();
        QVERIFY2(ctx.cancelAt() != Clock::time_point{}, "the stage never came");
        QCOMPARE(r.status, Status::Cancelled);
        const double ms = std::chrono::duration<double, std::milli>(took).count();
        QVERIFY2(ms < 250, qPrintable(QStringLiteral("Stop took %1 ms").arg(ms, 0, 'f', 1)));
        if (stage == QLatin1StringView("Primes")) {
            QVERIFY(r.metric(QStringLiteral("int.rate")));  // the integer group finished
            QVERIFY(!r.metric(QStringLiteral("fp.primes")));
            QCOMPARE(r.subResults.first().status, Status::Ok);
            QCOMPARE(r.subResults.last().status, Status::Cancelled);
        }
    }

    // A group that fails (here: the encryption worker can't start) is an
    // Error; the others run; the test is Partial with no score.
    void failingGroupIsPartial()
    {
        cpu::CryptoClient::setWorkerForTests(QStringLiteral("/nonexistent/synbench-cpucrypto"));
        tests::CpuTest t;
        FakeRunContext ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QCOMPARE(r.statusReason, QStringLiteral("No CPU score: not every sub-test ran"));
        for (const SubResult &s : r.subResults)
            QCOMPARE(s.status, s.id == QLatin1StringView("crypto") ? Status::Error : Status::Ok);
        QVERIFY(r.metric(QStringLiteral("physics.nbody")));
        QVERIFY(!r.metric(QStringLiteral("crypto.aes_gcm")));
    }

    // Task 16: the Detail page of the approved mockup, from the mockup's
    // own (illustrative) numbers.
    void detailCompleted()
    {
        TestResult r = mockupResult();
        tests::CpuTest t;
        const DetailLayout d = t.detail(r);
        QCOMPARE(d.subtitle, QStringLiteral("Score 875 · AVX-512 path · 24 threads · Throttling detected"));
        QCOMPARE(d.headlineMetrics, QStringList{QStringLiteral("single.mark")});
        // The order: KPIs, groups, throttle note, Encryption, its note,
        // Compression + Single-thread (a pair), the two charts.
        QStringList kinds;
        for (const DetailBlock &b : d.blocks)
            kinds << (std::holds_alternative<KpisBlock>(b.content) ? QStringLiteral("kpis")
                      : std::holds_alternative<TableBlock>(b.content) ? QStringLiteral("table:") + b.title
                      : std::holds_alternative<NoteBlock>(b.content) ? QStringLiteral("note")
                      : std::holds_alternative<LineChartBlock>(b.content) ? QStringLiteral("chart:") + b.title
                                                                           : QStringLiteral("?"));
        QCOMPARE(kinds, (QStringList{QStringLiteral("kpis"), QStringLiteral("table:Sub-tests (all threads)"), QStringLiteral("note"),
                                     QStringLiteral("table:Encryption"), QStringLiteral("note"),
                                     QStringLiteral("table:Compression (MB/s)"),
                                     QStringLiteral("table:Single-thread · pinned per repetition (CPU 0, 5)"),
                                     QStringLiteral("chart:Clock (CPUs running the test, MHz)"),
                                     QStringLiteral("chart:Temperature (k10temp Tctl, °C)")}));
        const auto &kpis = std::get<KpisBlock>(d.blocks[0].content).items;
        QCOMPARE(kpis.size(), 5);
        QCOMPARE(kpis[1].value.text, QStringLiteral("AVX-512"));
        QCOMPARE(kpis[1].unitOverride, QStringLiteral("512-bit"));
        QCOMPARE(kpis[2].value.text, QStringLiteral("AES-NI"));
        QCOMPARE(kpis[2].unitOverride, QStringLiteral("+ VAES · SHA-NI"));
        QCOMPARE(kpis[3].unitOverride, QStringLiteral("logical CPUs"));
        QCOMPARE(kpis[4].value.text, QStringLiteral("95 °C"));
        QCOMPARE(kpis[4].unitOverride, QStringLiteral("k10temp Tctl"));
        const auto &groups = std::get<TableBlock>(d.blocks[1].content);
        QCOMPARE(groups.rows.size(), 8);
        QCOMPARE(groups.rows[1][2].text, QStringLiteral("91,800 MFLOPS\nprimes 2,140 MOps/s"));
        QCOMPARE(groups.rows[2][0].text, QStringLiteral("SIMD (AVX-512)"));
        QCOMPARE(groups.rows[4][2].text, QStringLiteral("AES-256-GCM 96,400 MB/s\nSHA-256 24,300 MB/s"));
        QCOMPARE(groups.rows[7][2].text, QStringLiteral("weighted geometric mean"));
        QCOMPARE(groups.rows[7][3].text, QStringLiteral("875"));
        QCOMPARE(groups.rows[0][3].text, QStringLiteral("862"));
        QCOMPARE(std::get<NoteBlock>(d.blocks[2].content).text,
                 QStringLiteral("Throttling detected in xz compression: clock speed fell from 4.6 to 3.9 GHz (−15%) at 95 °C. "
                                "The score is not adjusted."));
        const auto &enc = std::get<TableBlock>(d.blocks[3].content);
        QCOMPARE(enc.rows[0][3].text, QStringLiteral("30.9×"));
        QCOMPARE(enc.rows[1][3].text, QStringLiteral("4.0×"));
        const auto &comp = std::get<TableBlock>(d.blocks[5].content);
        QCOMPARE(comp.rows[0][0].text, QStringLiteral("zlib (level 6)"));
        QCOMPARE(comp.rows[0][1].text, QStringLiteral("640"));
        QCOMPARE(comp.rows[0][2].text, QStringLiteral("6,380"));
        QCOMPARE(comp.rows[0][3].text, QStringLiteral("3.12"));
        const auto &st = std::get<TableBlock>(d.blocks[6].content);
        QCOMPARE(st.rows[3][1].text, QStringLiteral("921"));
        // The clock chart's labels: the throttled window replaces "Compression".
        const auto &clock = std::get<LineChartBlock>(d.blocks[7].content);
        QStringList labels;
        for (const ChartAnnotation &a : clock.annotations)
            labels << a.label;
        QCOMPARE(labels, (QStringList{QStringLiteral("Single-thread"), QStringLiteral("Integer"), QStringLiteral("xz (−15%)"),
                                      QStringLiteral("Encryption"), QStringLiteral("Physics")}));
        QCOMPARE(clock.xAxis.tickLabels, (QStringList{QStringLiteral("0 s"), QStringLiteral("60 s"), QStringLiteral("120 s"),
                                                      QStringLiteral("180 s"), QStringLiteral("240 s"), QStringLiteral("270 s")}));
        const auto &temp = std::get<LineChartBlock>(d.blocks[8].content);
        labels.clear();
        for (const ChartAnnotation &a : temp.annotations)
            labels << a.label;
        QCOMPARE(labels, (QStringList{QStringLiteral("Single-thread"), QStringLiteral("95 °C"), QStringLiteral("Encryption")}));
    }

    void detailPartialAndStopped()
    {
        tests::CpuTest t;
        TestResult r = mockupResult();
        // Encryption failed: Partial, no score.
        r.status = Status::Partial;
        r.score.reset();
        for (SubResult &s : r.subResults) {
            if (s.id == QLatin1StringView("crypto")) {
                s.status = Status::Error;
                s.reason = QStringLiteral("the worker crashed");
                s.score.reset();
            }
        }
        r.metrics.erase(std::remove_if(r.metrics.begin(), r.metrics.end(),
                                       [](const Metric &m) { return m.key.startsWith(QLatin1StringView("crypto.")); }),
                        r.metrics.end());
        DetailLayout d = t.detail(r);
        QVERIFY(d.subtitle.startsWith(QStringLiteral("No CPU score · ")));
        const auto &groups = std::get<TableBlock>(d.blocks[1].content);
        QCOMPARE(groups.rows[4][2].text, QStringLiteral("Error"));
        QCOMPARE(groups.rows[4][3].text, QStringLiteral("—"));
        QCOMPARE(groups.rows[7][3].text, QStringLiteral("—"));
        for (const DetailBlock &b : d.blocks)
            QVERIFY(b.title != QLatin1StringView("Encryption"));  // no table without numbers
        const auto *last = std::get_if<NoteBlock>(&d.blocks.last().content);
        QVERIFY(last && last->text.contains(QStringLiteral("crypto: the worker crashed")));
        // Stopped during sorting: what finished, no score.
        r = mockupResult();
        r.status = Status::Cancelled;
        r.score.reset();
        r.subResults.resize(5);
        r.subResults[4].status = Status::Cancelled;
        d = t.detail(r);
        QVERIFY(d.subtitle.startsWith(QStringLiteral("Stopped · ")));
        const auto &g2 = std::get<TableBlock>(d.blocks[1].content);
        QCOMPARE(g2.rows[5][2].text, QStringLiteral("Not run"));  // sorting
        QVERIFY(std::get<NoteBlock>(d.blocks.last().content).text.startsWith(QStringLiteral("Stopped before the end")));
    }

    // AP-5 M1: a single-thread part that fails makes the test Partial (the
    // mark is missing); the groups and the CPU score are unaffected.
    void singleThreadPartFails()
    {
        qputenv("SYNBENCH_CPU_FAIL", "Single-thread floating point");
        tests::CpuTest t;
        FakeRunContext ctx;
        const TestResult r = t.run(ctx, {});
        qunsetenv("SYNBENCH_CPU_FAIL");
        QCOMPARE(r.status, Status::Partial);
        QVERIFY2(r.statusReason.startsWith(QStringLiteral("single-thread mark incomplete: Single-thread floating point: ")),
                 qPrintable(r.statusReason));
        QVERIFY(!r.metric(QStringLiteral("single.fp")));
        QVERIFY(!r.metric(QStringLiteral("single.mark")));  // only with all three parts
        QVERIFY(r.metric(QStringLiteral("single.int")) && r.metric(QStringLiteral("single.compress")));
        for (const SubResult &s : r.subResults)
            QCOMPARE(s.status, Status::Ok);
        QVERIFY(t.score(r).total);
    }

    // AP-5 L2: a wrong result on the software encryption path is an Error,
    // like one on the hardware path (a worker that can't run there is only
    // a warning).
    void softwareWrongResultIsAnError()
    {
        qputenv("SYNBENCH_CPU_FAIL", "SHA-256 (software path)");
        tests::CpuTest t;
        FakeRunContext ctx;
        const TestResult r = t.run(ctx, {});
        qunsetenv("SYNBENCH_CPU_FAIL");
        QCOMPARE(r.status, Status::Partial);
        for (const SubResult &s : r.subResults)
            QCOMPARE(s.status, s.id == QLatin1StringView("crypto") ? Status::Error : Status::Ok);
        QVERIFY(r.metric(QStringLiteral("crypto.sha256")));  // the hardware path's numbers are kept
        QVERIFY(!r.metric(QStringLiteral("crypto.sha256.sw")));
    }

    // Dave / AP-5 L4: cores are listed in CPU order.
    void coreOrder()
    {
        QCOMPARE(tests::cpuListText({QStringLiteral("0")}), QStringLiteral("0"));
        QCOMPARE(tests::cpuListText({QStringLiteral("0"), QStringLiteral("5")}), QStringLiteral("0 and 5"));
        QCOMPARE(tests::cpuListText({QStringLiteral("0"), QStringLiteral("5"), QStringLiteral("7")}), QStringLiteral("0, 5 and 7"));
        TestResult r = mockupResult();
        for (Metric &m : r.metrics) {
            if (m.key == QLatin1StringView("single.cores"))
                m.value = QStringLiteral("integer 7,5,5,0,7 · FP 5,5,5,5,5 · compression 0,0,0,0,0");
        }
        tests::CpuTest t;
        const DetailLayout d = t.detail(r);
        QStringList titles;
        for (const DetailBlock &b : d.blocks)
            titles << b.title;
        QVERIFY2(titles.contains(QStringLiteral("Single-thread · pinned per repetition (CPU 0, 5, 7)")),
                 qPrintable(titles.join(QStringLiteral(" | "))));
    }

    void options()
    {
        tests::CpuTest t;
        struct Probe : ProbeContext {
            SystemSnapshot s;
            UnavailablePrivilegedOps p;
            const SystemSnapshot &system() const override { return s; }
            PrivilegedOps &privileged() const override { return const_cast<UnavailablePrivilegedOps &>(p); }
        } probe;
        const Availability a = t.probe(probe);
        QCOMPARE(a.state, Availability::State::Available);  // the worker is next to the tests
        const OptionSchema o = t.options(probe, a);
        for (const OptionDef &d : o.options)
            QCOMPARE(d.kind, OptionKind::Info);  // nothing to choose (D14)
        QVERIFY(o.find(QStringLiteral("threads")));
    }
};

QTEST_GUILESS_MAIN(TestCpuTest)
#include "tst_cpu_test.moc"
