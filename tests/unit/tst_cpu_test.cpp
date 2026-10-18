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
        QVERIFY(ctx.logged(QStringLiteral("xz compression: clock fell")));
        QVERIFY(ctx.logged(QStringLiteral("Throttling detected in xz compression")));
        QVERIFY(ctx.logged(QStringLiteral("at 95 °C")));
        QVERIFY(t.score(r).total);
    }

    // Stop in each phase: the run returns within 250 ms as Cancelled, with
    // the groups that finished.
    void stopInEachPhase_data()
    {
        QTest::addColumn<QString>("stage");
        for (const char *stage : {"Single-thread integer", "Primes", "zlib prepare", "xz compress", "AES-256-GCM", "Physics"})
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
