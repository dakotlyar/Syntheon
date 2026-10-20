// SPDX-License-Identifier: GPL-3.0-or-later
// The assembled RAM test (Phase 3 design task 9): a whole run at the
// developer time scale through a fake RunContext, its metrics, sub-results,
// series and log; Stop in each part within 250 ms; a failing part makes the
// test Partial; the memory readout from a snapshot (§7); and the TestInfo /
// options / estimate.
#include "core/units.h"
#include "isa/core_ranking.h"
#include "privileged/privileged_ops.h"
#include "tests/common/stub_clock.h"
#include "tests/cpu/engine/platform_threads.h"
#include "tests/cpu/engine/worker_pool.h"
#include "tests/ram/config/readout.h"
#include "tests/ram/loaded/load_generator.h"
#include "tests/ram/ram_test.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include <atomic>
#include <chrono>
#include <cmath>
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

    void setSystem(const SystemSnapshot &s) { m_system = s; }
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
    QStringList stages() const
    {
        std::lock_guard lock(m_mutex);
        QStringList out;
        for (const auto &p : m_progress)
            out << p.second;
        return out;
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

#if defined(__x86_64__) || defined(_M_X64)
constexpr bool kX64 = true;
#else
constexpr bool kX64 = false;
#endif

// Tiamat's memory as System info records it (dmidecode -t 17, 2026-10-19;
// no serials anywhere in a snapshot, P23).
SystemSnapshot tiamatSnapshot()
{
    const auto module = [](const char *locator, const char *bank) {
        return QJsonObject{{QStringLiteral("size_bytes"), 16.0 * 1024 * 1024 * 1024},
                           {QStringLiteral("type"), QStringLiteral("DDR5")},
                           {QStringLiteral("speed_mts"), 6000},
                           {QStringLiteral("configured_speed_mts"), 6000},
                           {QStringLiteral("manufacturer"), QStringLiteral("G.Skill")},
                           {QStringLiteral("manufacturer_source"), QStringLiteral("jedec")},
                           {QStringLiteral("part_number"), QStringLiteral("F5-6000J3038F16G")},
                           {QStringLiteral("locator"), QLatin1StringView(locator)},
                           {QStringLiteral("bank_locator"), QLatin1StringView(bank)},
                           {QStringLiteral("rank"), 2},
                           {QStringLiteral("data_width"), 64},
                           {QStringLiteral("total_width"), 64},
                           {QStringLiteral("form_factor"), QStringLiteral("DIMM")}};
    };
    SystemSnapshot s;
    s.data.insert(QStringLiteral("memory"),
                  QJsonObject{{QStringLiteral("total_bytes"), 32.0 * 1024 * 1024 * 1024},
                              {QStringLiteral("modules_known"), true},
                              {QStringLiteral("modules_source"), QStringLiteral("udev")},
                              {QStringLiteral("channels"), 2},
                              {QStringLiteral("modules"), QJsonArray{module("DIMMA2", "P0 CHANNEL A"), module("DIMMB2", "P0 CHANNEL B")}}});
    return s;
}

QString textOf(const TestResult &r, const char *key)
{
    const Metric *m = r.metric(QLatin1StringView(key));
    return m && !m->isNumeric() ? std::get<QString>(m->value) : QString();
}

double numberOf(const TestResult &r, const char *key)
{
    const Metric *m = r.metric(QLatin1StringView(key));
    return m && m->isNumeric() ? std::get<double>(m->value) : std::nan("");
}

} // namespace

class TestRamTest : public QObject
{
    Q_OBJECT

private slots:
    void init() { tests::setStubTimeScale(0.01); }
    void cleanup()
    {
        tests::setStubTimeScale(0);
        qunsetenv("SYNBENCH_RAM_FAIL");
    }

    void identity()
    {
        tests::RamTest t;
        QCOMPARE(t.info().id, QStringLiteral("ram"));
        QCOMPARE(t.info().colorToken, QStringLiteral("t-ram"));
        QCOMPARE(t.info().testVersion, 0);  // until calibration (task 14)
        QCOMPARE(t.info().formulaVersion, 0);
        QStringList subs;
        for (const SubTestInfo &s : t.info().subTests) {
            subs << s.id;
            QVERIFY(!s.userSelectable);  // D15: all or nothing
        }
        QCOMPARE(subs, (QStringList{QStringLiteral("latency"), QStringLiteral("bandwidth"), QStringLiteral("loaded")}));
        tests::setStubTimeScale(1);
        QCOMPARE(t.estimate({}), std::chrono::seconds(kX64 ? 160 : 125));
        QCOMPARE(t.timeLimit({}), std::chrono::seconds(kX64 ? 380 : 310));  // 2 x estimate + 60 s
    }

    // §2: info rows only (the stub's buffer and duration are retired), and
    // no Sub-tests checklist.
    void options()
    {
        tests::RamTest t;
        struct Ctx : ProbeContext {
            SystemSnapshot s = tiamatSnapshot();
            UnavailablePrivilegedOps p;
            const SystemSnapshot &system() const override { return s; }
            PrivilegedOps &privileged() const override { return const_cast<UnavailablePrivilegedOps &>(p); }
        } ctx;
        const Availability a = t.probe(ctx);
        QVERIFY(a.state != Availability::State::Unavailable);
        bool pages = false;
        for (const CapabilityItem &c : a.capabilities)
            pages = pages || c.label == QLatin1StringView("Huge pages") || c.label == QLatin1StringView("4 KiB pages");
        QVERIFY(pages);
        const OptionSchema s = effectiveOptionSchema(t, ctx, a);
        QStringList keys;
        for (const OptionDef &o : s.options) {
            keys << o.key;
            QCOMPARE(o.kind, OptionKind::Info);
        }
        QVERIFY(!keys.contains(QStringLiteral("buffer")) && !keys.contains(QStringLiteral("duration")));
        QVERIFY(!keys.contains(QStringLiteral("subtests")));
        QCOMPARE(s.find(QStringLiteral("memory"))->infoValue, QStringLiteral("DDR5-6000 · 2 × 16 GB · 2 channels (SMBIOS)"));
        QVERIFY(s.find(QStringLiteral("pages")));
        QVERIFY(s.find(QStringLiteral("threads"))->infoValue.startsWith(QLatin1StringView("All ")));
        QVERIFY(s.find(QStringLiteral("runs"))->infoValue.contains(QLatin1StringView("every part")));
    }

    // §7, P16, D12: the readout's metrics from Tiamat's snapshot.
    void readout()
    {
        const ram::MemoryReadout r = ram::memoryReadout(tiamatSnapshot(), false);
        QVERIFY(r.available);
        TestResult t;
        t.metrics = r.metrics;
        QCOMPARE(numberOf(t, "speed"), 6000.0);
        QCOMPARE(numberOf(t, "channels"), 2.0);
        QCOMPARE(textOf(t, "config.type"), QStringLiteral("DDR5"));
        QCOMPARE(textOf(t, "config.total"), QStringLiteral("32 GB"));
        QCOMPARE(textOf(t, "config.modules"), QStringLiteral("2 × 16 GB"));
        QCOMPARE(textOf(t, "config.ranks"), QStringLiteral("2R"));
        QCOMPARE(textOf(t, "config.width"), QStringLiteral("128-bit"));
        QCOMPARE(textOf(t, "config.rated"), QStringLiteral("6,000 MT/s"));
        QCOMPARE(textOf(t, "config.source"), QStringLiteral("SMBIOS (udev)"));
        QCOMPARE(numberOf(t, "config.peak"), 96.0);  // 6,000 MT/s x 128-bit / 8
        QVERIFY(r.peakGBs && *r.peakGBs == 96.0);
        QCOMPARE(textOf(t, "config.module.1"),
                 QStringLiteral("DIMMA2 (P0 CHANNEL A) · 16 GB · DDR5 · 6,000 MT/s · 2R · G.Skill · F5-6000J3038F16G"));
        QCOMPARE(ram::moduleFields(textOf(t, "config.module.2")).size(), ram::kModuleFields);
        QVERIFY(!t.metric(QStringLiteral("config.module.3")));
        // Mixed modules: the lowest speed, both sizes, both rank counts; one
        // without a width: no bus width and no peak.
        SystemSnapshot mixed = tiamatSnapshot();
        QJsonObject memory = mixed.data.value(QStringLiteral("memory")).toObject();
        QJsonArray modules = memory.value(QStringLiteral("modules")).toArray();
        QJsonObject second = modules[1].toObject();
        second.insert(QStringLiteral("size_bytes"), 8.0 * 1024 * 1024 * 1024);
        second.insert(QStringLiteral("configured_speed_mts"), 4800);
        second.insert(QStringLiteral("rank"), 1);
        second.remove(QStringLiteral("data_width"));
        second.remove(QStringLiteral("manufacturer"));
        modules[1] = second;
        memory.insert(QStringLiteral("modules"), modules);
        memory.insert(QStringLiteral("modules_source"), QStringLiteral("smbios"));
        mixed.data.insert(QStringLiteral("memory"), memory);
        const ram::MemoryReadout m = ram::memoryReadout(mixed, false);
        t.metrics = m.metrics;
        QCOMPARE(numberOf(t, "speed"), 4800.0);
        QCOMPARE(textOf(t, "config.modules"), QStringLiteral("1 × 16 GB + 1 × 8 GB"));
        QCOMPARE(textOf(t, "config.ranks"), QStringLiteral("1R/2R"));
        QCOMPARE(textOf(t, "config.source"), QStringLiteral("SMBIOS (System info)"));
        QVERIFY(!t.metric(QStringLiteral("config.width")));
        QVERIFY(!m.peakGBs);
        QCOMPARE(ram::moduleFields(textOf(t, "config.module.2")).at(5), QStringLiteral("—"));
        // Nothing known: the reasons (§7) and no numbers.
        SystemSnapshot none;
        none.data.insert(QStringLiteral("memory"), QJsonObject{{QStringLiteral("modules_known"), false}});
        const ram::MemoryReadout n = ram::memoryReadout(none, false);
        QVERIFY(!n.available);
        QCOMPARE(n.reason, QStringLiteral("the SMBIOS helper wasn't authorised and udev has no memory data"));
        t.metrics = n.metrics;
        QVERIFY(textOf(t, "config.source").startsWith(QLatin1StringView("not available: ")));
        QVERIFY(!t.metric(QStringLiteral("speed")));
        QCOMPARE(ram::memoryReadout(none, true).reason, QStringLiteral("the firmware's SMBIOS tables couldn't be read"));
        none.data.insert(QStringLiteral("memory"), QJsonObject{{QStringLiteral("modules_known"), true}});
        QCOMPARE(ram::memoryReadout(none, false).reason, QStringLiteral("the firmware lists no memory devices"));
    }

    // A whole run at 1% of real time (the buffers keep their real sizes).
    void wholeRun()
    {
        tests::RamTest t;
        FakeRunContext ctx;
        ctx.setSystem(tiamatSnapshot());
        const auto start = Clock::now();
        const TestResult r = t.run(ctx, {});
        qInfo("whole run at 1%%: %.1f s", std::chrono::duration<double>(Clock::now() - start).count());
        QVERIFY2(r.status == Status::Ok, qPrintable(r.statusReason + QStringLiteral("\n") + ctx.allLogs()));
        // The scored metrics (§2), with their units and directions.
        QStringList scored = {QStringLiteral("latency.l1"), QStringLiteral("latency.l2"), QStringLiteral("latency.dram"),
                              QStringLiteral("loaded.latency")};
        const bool l3 = textOf(r, "cache.levels").contains(QLatin1StringView("L3 ")) && r.metric(QStringLiteral("latency.l3"));
        if (l3)
            scored << QStringLiteral("latency.l3");
        for (const QString &k : scored) {
            const Metric *m = r.metric(k);
            QVERIFY2(m && m->isNumeric() && std::get<double>(m->value) > 0, qPrintable(k));
            QCOMPARE(m->unit, QStringLiteral("ns"));
            QCOMPARE(m->direction, Direction::LowerIsBetter);
        }
        for (const char *op : {"read", "write", "copy"}) {
            for (const char *prefix : {"bandwidth.", "bandwidth.st."}) {
                const QString k = QLatin1StringView(prefix) + QLatin1StringView(op);
                const Metric *m = r.metric(k);
                QVERIFY2(m && std::get<double>(m->value) > 0, qPrintable(k));
                QCOMPARE(m->unit, QStringLiteral("GB/s"));
                QCOMPARE(m->direction, Direction::HigherIsBetter);
            }
        }
        // D4: the plain-store pair on x64 only, never scored.
        for (const char *k : {"bandwidth.write.plain", "bandwidth.copy.plain", "bandwidth.st.write.plain", "bandwidth.st.copy.plain"}) {
            const Metric *m = r.metric(QLatin1StringView(k));
            QCOMPARE(m != nullptr, kX64);
            if (m)
                QCOMPARE(m->direction, Direction::Neutral);
        }
        // Headlines: DRAM latency, all-thread read, loaded latency.
        for (const char *k : {"latency.dram", "bandwidth.read", "loaded.latency"})
            QVERIFY2(r.metric(QLatin1StringView(k))->headline, k);
        QVERIFY(numberOf(r, "latency.l1") < numberOf(r, "latency.dram"));
        QVERIFY(numberOf(r, "latency.l1") > 0.2 && numberOf(r, "latency.dram") < 1000);
        for (const char *k : {"latency.l1.at", "latency.l2.at", "latency.dram.at", "cache.levels", "latency.core",
                              "bandwidth.path", "bandwidth.buffer", "bandwidth.st.core", "loaded.core", "pages", "power.source",
                              "config.type", "config.module.1"})
            QVERIFY2(!textOf(r, k).isEmpty(), k);
        QVERIFY(textOf(r, "latency.dram.at").endsWith(QLatin1StringView("GiB")));
        for (const char *k : {"pages.huge", "threads", "numa.nodes", "stability.spread", "loaded.load", "loaded.threads",
                              "config.peak", "config.efficiency", "background.load"})
            QVERIFY2(std::isfinite(numberOf(r, k)), k);
        QVERIFY(numberOf(r, "loaded.threads") >= 1 && numberOf(r, "loaded.threads") < numberOf(r, "threads"));
        // The series: the latency curve ascending, never a zero (a log axis),
        // with every target on it; the loaded curve, idle point first.
        const Series *curve = nullptr, *loaded = nullptr;
        for (const Series &s : r.series) {
            if (s.key == QLatin1StringView("latency_curve"))
                curve = &s;
            if (s.key == QLatin1StringView("loaded_curve"))
                loaded = &s;
        }
        QVERIFY(curve && loaded);
        QVERIFY(curve->points.size() >= 37);
        for (qsizetype i = 0; i < curve->points.size(); ++i) {
            QVERIFY(curve->points[i].y() > 0);
            if (i > 0)
                QVERIFY(curve->points[i].x() > curve->points[i - 1].x());
        }
        QCOMPARE(curve->points.last().y(), numberOf(r, "latency.dram"));
        QCOMPARE(loaded->points.size(), 6);
        QCOMPARE(loaded->points.first().x(), 0.0);
        QCOMPARE(loaded->points.first().y(), numberOf(r, "latency.dram"));
        // The parts, in order, all Ok.
        QStringList ids;
        for (const SubResult &s : r.subResults) {
            ids << s.id;
            QCOMPARE(s.status, Status::Ok);
        }
        QCOMPARE(ids, (QStringList{QStringLiteral("latency"), QStringLiteral("bandwidth"), QStringLiteral("loaded")}));
        QCOMPARE(ctx.subTests(), (QStringList{QStringLiteral("latency:begin"), QStringLiteral("latency:ok"),
                                              QStringLiteral("bandwidth:begin"), QStringLiteral("bandwidth:ok"),
                                              QStringLiteral("loaded:begin"), QStringLiteral("loaded:ok")}));
        // Progress only goes forward and ends at 1; the mockup's stage line.
        QVERIFY(!ctx.wentBackwards());
        QCOMPARE(ctx.progressCalls().last().first, 1.0);
        if (kX64)
            QVERIFY(ctx.stages().contains(QStringLiteral("Write, plain stores · rep 2 of 5")));
        for (const char *line : {"Starting RAM: ", "Pages: ", "Latency on ", "Latency curve: ", "Latency L1 ", "Single thread on ",
                                 "Single thread: read ", "All threads: ", "Read ", "Loaded latency on ", "Full load: "})
            QVERIFY2(ctx.logged(QLatin1StringView(line)), line);
        QCOMPARE(ram::LoadGenerator::liveThreads(), 0);
        QCOMPARE(cpu::WorkerPool::liveThreads(), 0);
    }

    void stopInEachPart_data()
    {
        QTest::addColumn<QString>("stage");
        for (const char *stage : {"Preparing buffers", "Latency curve · 4 KiB", "Latency curve · 1 GiB", "Latency L2",
                                  "Preparing bandwidth buffers", "Single-thread read", "Copy", "Loaded latency · preparing",
                                  "Loaded latency, full load", "Loaded latency, 2,000 ns delay"})
            QTest::newRow(stage) << QString::fromUtf8(stage);
    }
    void stopInEachPart()
    {
        QFETCH(QString, stage);
        tests::setStubTimeScale(0.02);  // windows long enough to stop inside
        tests::RamTest t;
        FakeRunContext ctx;
        ctx.cancelOn(stage);
        const TestResult r = t.run(ctx, {});
        const auto took = Clock::now() - ctx.cancelAt();
        QVERIFY2(ctx.cancelAt() != Clock::time_point{}, "the stage never came");
        QCOMPARE(r.status, Status::Cancelled);
        const double ms = std::chrono::duration<double, std::milli>(took).count();
        qInfo("Stop at %s: %.1f ms", qPrintable(stage), ms);
        QVERIFY2(ms < 250, qPrintable(QStringLiteral("Stop took %1 ms").arg(ms, 0, 'f', 1)));
        QCOMPARE(ram::LoadGenerator::liveThreads(), 0);
        QCOMPARE(cpu::WorkerPool::liveThreads(), 0);
        // The part that was running says Stopped; one not started isn't listed.
        if (stage != QLatin1StringView("Preparing buffers"))
            QCOMPARE(r.subResults.last().status, Status::Cancelled);
        else
            QVERIFY(r.subResults.isEmpty());
    }

    // A part whose measurement fails is an Error with the reason; the others
    // run; the test is Partial.
    void failingPartIsPartial_data()
    {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<QString>("part");
        QTest::newRow("latency") << QStringLiteral("Latency L2") << QStringLiteral("latency");
        QTest::newRow("bandwidth") << QStringLiteral("Copy") << QStringLiteral("bandwidth");
        QTest::newRow("loaded") << QStringLiteral("Loaded latency") << QStringLiteral("loaded");
    }
    void failingPartIsPartial()
    {
        QFETCH(QString, stage);
        QFETCH(QString, part);
        qputenv("SYNBENCH_RAM_FAIL", stage.toUtf8());
        tests::RamTest t;
        FakeRunContext ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QCOMPARE(r.statusReason, QStringLiteral("No RAM score: not every part ran"));
        QCOMPARE(r.subResults.size(), 3);
        for (const SubResult &s : r.subResults) {
            QCOMPARE(s.status, s.id == part ? Status::Error : Status::Ok);
            if (s.id == part)
                QVERIFY2(s.reason.contains(QLatin1StringView("forced wrong result")), qPrintable(s.reason));
        }
        QVERIFY(ctx.logged(QStringLiteral("forced wrong result")));
        if (part == QLatin1StringView("latency")) {
            QVERIFY(r.metric(QStringLiteral("latency.l1")));
            QVERIFY(!r.metric(QStringLiteral("latency.dram")));
        }
    }
};

QTEST_GUILESS_MAIN(TestRamTest)
#include "tst_ram_test.moc"
