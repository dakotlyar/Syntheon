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
#include "tests/ram/formula_v1.h"
#include "tests/ram/loaded/load_generator.h"
#include "tests/ram/ram_test.h"

#include "core/detail.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#endif

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
        // A stage, or "<part>: <stage>" (the bandwidth parts share stages).
        const QString withPart = (m_parts.isEmpty() ? QString() : m_parts.last()) + QStringLiteral(": ") + stage;
        if (!m_cancelOn.isEmpty() && (stage.startsWith(m_cancelOn) || withPart.startsWith(m_cancelOn)) && !m_cancel && !m_armed) {
            if (m_cancelDelay.count() == 0) {
                m_cancelAt = Clock::now();
                m_cancel = true;
            } else {
                // Stop a while into the stage (the warm-up's 2 s cap).
                m_armed = true;
                m_delayed = std::thread([this] {
                    std::this_thread::sleep_for(m_cancelDelay);
                    m_cancelAt = Clock::now();
                    m_cancel = true;
                });
            }
        }
    }
    void log(LogLevel level, const QString &message) override
    {
        std::lock_guard lock(m_mutex);
        m_logs.append({level, message});
    }
    void setPart(const QString &part) override
    {
        std::lock_guard lock(m_mutex);
        if (m_parts.isEmpty() || m_parts.last() != part)
            m_parts.append(part);
    }
    QStringList parts() const
    {
        std::lock_guard lock(m_mutex);
        return m_parts;
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
    void cancelOn(const QString &stage, std::chrono::milliseconds delay = {})
    {
        m_cancelOn = stage;
        m_cancelDelay = delay;
    }
    ~FakeRunContext() override
    {
        if (m_delayed.joinable())
            m_delayed.join();
    }
    Clock::time_point cancelAt() const { return m_cancelAt.load(); }
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
    QStringList m_parts;
    std::atomic<bool> m_cancel{false};
    std::atomic<Clock::time_point> m_cancelAt{};  // set from the delayed-stop thread (AP-5 round 4 L6)
    QString m_cancelOn;
    std::chrono::milliseconds m_cancelDelay{0};
    bool m_armed = false;
    std::thread m_delayed;
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

// The approved mockup's completed run (all numbers illustrative, as drawn:
// "Example desktop", DDR5-6000 2 x 16 GB, CPU 4, 2 MiB pages).
TestResult mockupResult()
{
    TestResult r;
    r.testId = QStringLiteral("ram");
    r.status = Status::Ok;
    r.score = 1186;
    const auto num = [&](const char *key, const char *sub, double v, const char *unit) {
        r.metrics.append({QLatin1StringView(key), QLatin1StringView(sub), QString(), v, QLatin1StringView(unit), Direction::Neutral, false});
    };
    const auto txt = [&](const char *key, const QString &v) {
        r.metrics.append({QLatin1StringView(key), {}, QString(), v, {}, Direction::Neutral, false});
    };
    num("latency.l1", "latency", 0.8, "ns");
    num("latency.l2", "latency", 2.6, "ns");
    num("latency.l3", "latency", 9.6, "ns");
    num("latency.dram", "latency", 74.2, "ns");
    txt("latency.l1.at", QStringLiteral("24 KiB"));
    txt("latency.l2.at", QStringLiteral("220 KiB"));
    txt("latency.l3.at", QStringLiteral("5.7 MiB"));
    txt("latency.dram.at", QStringLiteral("1 GiB"));
    txt("cache.levels", QStringLiteral("L1d 48 KiB · L2 1 MiB · L3 32 MiB (shared by CPUs 0–7, 16–23)"));
    txt("latency.core", QStringLiteral("CPU 4 (top-ranked core, quietest of CPUs 2, 4)"));
    txt("bandwidth.st.core", QStringLiteral("CPU 4 (top-ranked core, quietest of CPUs 2, 4)"));
    txt("loaded.core", QStringLiteral("CPU 4 (top-ranked core, quietest of CPUs 2, 4)"));
    num("bandwidth.read", "bandwidth", 72.4, "GB/s");
    num("bandwidth.write", "bandwidth", 61.8, "GB/s");
    num("bandwidth.copy", "bandwidth", 66.5, "GB/s");
    num("bandwidth.write.plain", "bandwidth", 41.3, "GB/s");
    num("bandwidth.copy.plain", "bandwidth", 52.6, "GB/s");
    num("bandwidth.st.read", "bandwidth", 41.0, "GB/s");
    num("bandwidth.st.write", "bandwidth", 28.6, "GB/s");
    num("bandwidth.st.copy", "bandwidth", 36.2, "GB/s");
    num("bandwidth.st.write.plain", "bandwidth", 22.4, "GB/s");
    num("bandwidth.st.copy.plain", "bandwidth", 27.9, "GB/s");
    txt("bandwidth.path", QStringLiteral("AVX2 loads; write/copy scored with non-temporal stores, plain stores also shown"));
    num("loaded.latency", "loaded", 138, "ns");
    num("loaded.load", "loaded", 68.6, "GB/s");
    num("loaded.threads", "loaded", 30, "");
    num("threads", "", 32, "");
    num("speed", "", 6000, "MT/s");
    num("channels", "", 2, "");
    txt("config.type", QStringLiteral("DDR5"));
    txt("config.total", QStringLiteral("32 GB"));
    txt("config.modules", QStringLiteral("2 × 16 GB"));
    txt("config.ranks", QStringLiteral("2R"));
    txt("config.width", QStringLiteral("128-bit"));
    txt("config.rated", QStringLiteral("6,000 MT/s"));
    txt("config.source", QStringLiteral("SMBIOS (System info)"));
    num("config.peak", "", 96.0, "GB/s");
    num("config.efficiency", "", 72.4 / 96.0 * 100, "%");
    txt("config.module.1", QStringLiteral("DIMMA2 (P0 CHANNEL A) · 16 GB · DDR5 · 6,000 MT/s · 2R · G.Skill · EX6000C30-16G"));
    txt("config.module.2", QStringLiteral("DIMMB2 (P0 CHANNEL B) · 16 GB · DDR5 · 6,000 MT/s · 2R · G.Skill · EX6000C30-16G"));
    txt("pages", QStringLiteral("2 MiB transparent huge pages"));
    num("pages.huge", "", 100, "%");
    txt("power.source", QStringLiteral("AC"));
    num("background.load", "", 2, "%");
    r.subResults = {{QStringLiteral("latency"), Status::Ok, {}, 1567.0},
                    {QStringLiteral("bandwidth"), Status::Ok, {}, 716.0},
                    {QStringLiteral("loaded"), Status::Ok, {}, 1522.0}};
    Series curve{QStringLiteral("latency_curve"), QString(), QString(), QStringLiteral("KiB"), QStringLiteral("ns"), {}};
    for (const auto &[kib, nsv] : std::initializer_list<std::pair<double, double>>{
             {4, 0.8}, {16, 0.8}, {24, 0.8}, {48, 0.9}, {64, 1.4}, {128, 2.5}, {220, 2.6}, {512, 2.7}, {1024, 3.5}, {2048, 8},
             {5836.8, 9.6}, {16384, 10.5}, {32768, 25}, {131072, 60}, {1048576, 74.2}})
        curve.points.append({kib, nsv});
    Series loaded{QStringLiteral("loaded_curve"), QString(), QString(), QStringLiteral("GB/s"), QStringLiteral("ns"),
                  {{0, 74.2}, {19.6, 76.8}, {52.4, 88.5}, {64.9, 109}, {67.8, 127}, {68.6, 138}}};
    r.series = {curve, loaded};
    return r;
}

const TableBlock *tableTitled(const DetailLayout &d, const QString &title)
{
    for (const DetailBlock &b : d.blocks) {
        if (b.title == title)
            return std::get_if<TableBlock>(&b.content);
    }
    return nullptr;
}

const LineChartBlock *chartTitled(const DetailLayout &d, const QString &prefix)
{
    for (const DetailBlock &b : d.blocks) {
        if (b.title.startsWith(prefix))
            return std::get_if<LineChartBlock>(&b.content);
    }
    return nullptr;
}

QStringList notes(const DetailLayout &d, Tone *firstTone = nullptr)
{
    QStringList out;
    for (const DetailBlock &b : d.blocks) {
        if (const auto *n = std::get_if<NoteBlock>(&b.content)) {
            if (out.isEmpty() && firstTone)
                *firstTone = n->tone;
            out << n->text;
        }
    }
    return out;
}

QStringList cells(const TableBlock &t, int row)
{
    QStringList out;
    for (const DetailCell &c : t.rows.value(row))
        out << c.text;
    return out;
}

QStringList titles(const DetailLayout &d)
{
    QStringList out;
    for (const DetailBlock &b : d.blocks) {
        if (std::holds_alternative<KpisBlock>(b.content))
            out << QStringLiteral("[kpis]");
        else if (std::holds_alternative<NoteBlock>(b.content))
            out << QStringLiteral("[note]");
        else
            out << b.title;
    }
    return out;
}

} // namespace

class TestRamTest : public QObject
{
    Q_OBJECT

private slots:
    // What the runner gives (CI's tst_ram_test output on Windows wasn't
    // kept once; this says the machine's shape up front).
    void initTestCase()
    {
        const std::vector<isa::CoreChoice> allowed = isa::allowedLogicalCpus();
        qInfo("allowed logical CPUs: %d; single-thread core: CPU %d", int(allowed.size()), isa::bestCoreForSingleThread().cpu);
    }
    void init()
    {
        tests::setStubTimeScale(0.01);
        // A runner with one physical core (CI: two SMT threads) would make
        // loaded latency N/A and skip its paths; the load shares the core
        // there instead (a dev hook; checkpoint 3 CI). It changes nothing
        // where the load has a core of its own.
        qputenv("SYNBENCH_RAM_LOAD_SHARES_CORE", "1");
    }
    void cleanup()
    {
        tests::setStubTimeScale(0);
        qunsetenv("SYNBENCH_RAM_FAIL");
        qunsetenv("SYNBENCH_RAM_LOAD_SHARES_CORE");
    }

    // One physical core (this thread's affinity cut to the first allowed
    // CPU and its SMT siblings, as on CI's 2-thread runners): without the
    // hook loaded latency is N/A "needs at least two cores" and the other
    // parts run; with it, the load shares the core and loaded runs.
    void loadedOnOneCore_data()
    {
        QTest::addColumn<bool>("hook");
        QTest::newRow("product rule") << false;
        QTest::newRow("test hook") << true;
    }
    void loadedOnOneCore()
    {
#if !defined(__linux__)
        QSKIP("cutting the affinity in-process is done on Linux only; the hook itself runs in every test here");
#else
        QFETCH(bool, hook);
        if (!hook)
            qunsetenv("SYNBENCH_RAM_LOAD_SHARES_CORE");
        cpu_set_t saved;
        QVERIFY(sched_getaffinity(0, sizeof(saved), &saved) == 0);
        const std::vector<isa::CoreChoice> allowed = isa::allowedLogicalCpus();
        QVERIFY(!allowed.empty());
        cpu_set_t one;
        CPU_ZERO(&one);
        for (const int c : isa::linuxSmtSiblingKeys("/sys", allowed.front().cpu))
            CPU_SET(c, &one);
        QVERIFY(sched_setaffinity(0, sizeof(one), &one) == 0);
        struct Restore {
            cpu_set_t set;
            ~Restore() { sched_setaffinity(0, sizeof(set), &set); }
        } restore{saved};
        tests::RamTest t;
        FakeRunContext ctx;
        const TestResult r = t.run(ctx, {});
        const SubResult *loaded = nullptr;
        for (const SubResult &s : r.subResults) {
            if (s.id == QLatin1StringView("loaded"))
                loaded = &s;
            else
                QVERIFY2(s.status == Status::Ok, qPrintable(s.id + QStringLiteral(": ") + s.reason + QStringLiteral("\n") + ctx.allLogs()));
        }
        QVERIFY(loaded);
        if (hook) {
            QVERIFY2(loaded->status == Status::Ok, qPrintable(loaded->reason + QStringLiteral("\n") + ctx.allLogs()));
            QVERIFY(ctx.logged(QStringLiteral("SYNBENCH_RAM_LOAD_SHARES_CORE")));
        } else {
            QCOMPARE(loaded->status, Status::NotAvailable);
            QCOMPARE(loaded->reason, QStringLiteral("needs at least two cores"));
            QVERIFY(!ctx.logged(QStringLiteral("SYNBENCH_RAM_LOAD_SHARES_CORE")));
        }
        QCOMPARE(ram::LoadGenerator::liveThreads(), 0);
        QCOMPARE(cpu::WorkerPool::liveThreads(), 0);
#endif
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
        // Decision 4 (AP-5 M3): 4 x DDR5 on a dual-channel board is a 128-bit
        // bus (96 GB/s at 6,000), not 256: two DIMMs share a channel.
        const auto dimm = [](const char *locator, const char *bank, int width) {
            return QJsonObject{{QStringLiteral("size_bytes"), 16.0 * 1024 * 1024 * 1024}, {QStringLiteral("type"), QStringLiteral("DDR5")},
                               {QStringLiteral("speed_mts"), 6000}, {QStringLiteral("configured_speed_mts"), 6000},
                               {QStringLiteral("locator"), QLatin1StringView(locator)}, {QStringLiteral("bank_locator"), QLatin1StringView(bank)},
                               {QStringLiteral("rank"), 2}, {QStringLiteral("data_width"), width},
                               {QStringLiteral("part_number"), QStringLiteral("F5-6000 · 32G")}};
        };
        const auto snapshot = [](const QJsonArray &mods, int channels) {
            SystemSnapshot s;
            s.data.insert(QStringLiteral("memory"), QJsonObject{{QStringLiteral("modules_known"), true}, {QStringLiteral("channels"), channels},
                                                                {QStringLiteral("modules"), mods}});
            return s;
        };
        const auto widthOf = [&](const ram::MemoryReadout &m) {
            TestResult x;
            x.metrics = m.metrics;
            return textOf(x, "config.width");
        };
        ram::MemoryReadout four = ram::memoryReadout(
            snapshot({dimm("DIMMA1", "P0 CHANNEL A", 64), dimm("DIMMA2", "P0 CHANNEL A", 64), dimm("DIMMB1", "P0 CHANNEL B", 64),
                      dimm("DIMMB2", "P0 CHANNEL B", 64)}, 2), false);
        QCOMPARE(widthOf(four), QStringLiteral("128-bit"));
        QCOMPARE(*four.peakGBs, 96.0);
        // The same from the locators alone ("DIMMA1" ... "DIMMB2").
        four = ram::memoryReadout(snapshot({dimm("DIMMA1", "", 64), dimm("DIMMA2", "", 64), dimm("DIMMB1", "", 64), dimm("DIMMB2", "", 64)}, 2), false);
        QCOMPARE(widthOf(four), QStringLiteral("128-bit"));
        // Locators that don't say: the channel estimate x the widest module.
        four = ram::memoryReadout(snapshot({dimm("Slot 1", "", 64), dimm("Slot 2", "", 64), dimm("Slot 3", "", 64), dimm("Slot 4", "", 64)}, 2), false);
        QCOMPARE(widthOf(four), QStringLiteral("128-bit"));
        // Soldered LPDDR5X (AP-5 M2): its widths add up, whatever the
        // locators say. Per channel, 32-bit each (a Strix Halo like
        // Knowhere: 8 x 32 = 256-bit) and 16-bit each (128-bit); 8 x 16-bit
        // under "BANK 0" .. "BANK 7", no channel named (128-bit, not the old
        // guess of 2 x 16); two x16 sub-channels under one channel locator
        // (32-bit, not 16).
        const auto chip = [&](const QString &bank, int width) {
            QJsonObject o = dimm("DIMM 0", qPrintable(bank), width);
            o.insert(QStringLiteral("type"), QStringLiteral("LPDDR5X"));
            o.insert(QStringLiteral("form_factor"), QStringLiteral("Row Of Chips"));
            return o;
        };
        QJsonArray lp, lp16, banks, sub;
        for (const char *ch : {"A", "B", "C", "D", "E", "F", "G", "H"}) {
            lp.append(chip(QStringLiteral("P0 CHANNEL %1").arg(QLatin1StringView(ch)), 32));
            lp16.append(chip(QStringLiteral("P0 CHANNEL %1").arg(QLatin1StringView(ch)), 16));
        }
        for (int i = 0; i < 8; ++i)
            banks.append(chip(QStringLiteral("BANK %1").arg(i), 16));
        sub.append(chip(QStringLiteral("P0 CHANNEL A"), 16));
        sub.append(chip(QStringLiteral("P0 CHANNEL A"), 16));
        QCOMPARE(widthOf(ram::memoryReadout(snapshot(lp, 8), false)), QStringLiteral("256-bit"));
        QCOMPARE(widthOf(ram::memoryReadout(snapshot(lp16, 8), false)), QStringLiteral("128-bit"));
        QCOMPARE(widthOf(ram::memoryReadout(snapshot(banks, 2), false)), QStringLiteral("128-bit"));
        QCOMPARE(widthOf(ram::memoryReadout(snapshot(sub, 1), false)), QStringLiteral("32-bit"));
        // An unknown form factor reads by type: LPDDR is soldered.
        QJsonArray untyped;
        for (int i = 0; i < 4; ++i) {
            QJsonObject o = chip(QStringLiteral("BANK %1").arg(i), 32);
            o.remove(QStringLiteral("form_factor"));
            untyped.append(o);
        }
        QCOMPARE(widthOf(ram::memoryReadout(snapshot(untyped, 2), false)), QStringLiteral("128-bit"));
        // Tiamat (one DIMM per channel) is unchanged above: 128-bit. And a
        // field holding the separator can't break the Modules row (AP-5 L5).
        TestResult x;
        x.metrics = four.metrics;
        const QStringList f = ram::moduleFields(textOf(x, "config.module.1"));
        QCOMPARE(f.size(), ram::kModuleFields);
        QCOMPARE(f.last(), QStringLiteral("F5-6000 / 32G"));
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

    // formula v1 (§10): at the anchors everything is 1,000; the weights act
    // as elasticities; no L3 moves its weight to the L2; a missing metric
    // means no total but keeps the other groups; only Ok gets a total.
    void formula()
    {
        const auto atAnchors = [](bool withL3) {
            TestResult r;
            r.status = Status::Ok;
            for (const GeometricGroup &g : ram::formula_v1::groups(withL3)) {
                for (const GeometricMetric &m : g.metrics)
                    r.metrics.append({m.metricKey, g.id, QString(), m.anchor, QString(), Direction::Neutral, false});
            }
            r.metrics.append({QStringLiteral("cache.levels"), QStringLiteral("latency"), QString(),
                              withL3 ? QStringLiteral("L1d 48 KiB · L2 1 MiB · L3 32 MiB (shared by CPUs 8–15, 24–31)")
                                     : QStringLiteral("L1d 96 KiB · L2 12 MiB (shared by CPUs 0–3)"),
                              {}, Direction::Neutral, false});
            return r;
        };
        const auto set = [](TestResult &r, const char *key, double factor) {
            for (Metric &m : r.metrics) {
                if (m.key == QLatin1StringView(key))
                    m.value = std::get<double>(m.value) * factor;
            }
        };
        tests::RamTest t;
        TestResult r = atAnchors(true);
        QVERIFY(ram::formula_v1::scoresL3(r));
        ScoreBreakdown b = t.score(r);
        QCOMPARE(*b.total, 1000.0);
        QCOMPARE(b.subScores.size(), 3);
        for (const double s : b.subScores)
            QCOMPARE(s, 1000.0);
        // Group and metric weights sum to 1.
        for (const bool l3 : {true, false}) {
            double w = 0;
            for (const GeometricGroup &g : ram::formula_v1::groups(l3)) {
                w += g.weight;
                double v = 0;
                for (const GeometricMetric &m : g.metrics) {
                    v += m.weight;
                    QCOMPARE(m.lowerIsBetter, m.metricKey.startsWith(QLatin1StringView("latency."))
                                                  || m.metricKey == QLatin1StringView("loaded.latency"));
                }
                QVERIFY(std::abs(v - 1) < 1e-12);
            }
            QVERIFY(std::abs(w - 1) < 1e-12);
        }
        // DRAM latency halved: 1000 x 2^(0.50 x 0.75) (§10.2's +30%).
        set(r, "latency.dram", 0.5);
        b = t.score(r);
        QCOMPARE(*b.total, std::round(1000 * std::pow(2.0, 0.5 * 0.75)));
        QCOMPARE(b.subScores.value(QStringLiteral("latency")), std::round(1000 * std::pow(2.0, 0.75)));
        // All-thread read doubled: +5.5%; loaded latency halved: +11%.
        r = atAnchors(true);
        set(r, "bandwidth.read", 2);
        QCOMPARE(*t.score(r).total, std::round(1000 * std::pow(2.0, 0.35 * 2 / 9)));
        r = atAnchors(true);
        set(r, "loaded.latency", 0.5);
        QCOMPARE(*t.score(r).total, std::round(1000 * std::pow(2.0, 0.15)));
        // The plain-store metrics never count (D4).
        r = atAnchors(true);
        r.metrics.append({QStringLiteral("bandwidth.write.plain"), QStringLiteral("bandwidth"), QString(), 1.0, QString(),
                          Direction::Neutral, false});
        QCOMPARE(*t.score(r).total, 1000.0);
        // No L3 (D1, D2): L2 0.20; an L3 too close to the L2 counts as none.
        r = atAnchors(false);
        QVERIFY(!ram::formula_v1::scoresL3(r));
        QCOMPARE(*t.score(r).total, 1000.0);
        set(r, "latency.l2", 0.5);
        QCOMPARE(t.score(r).subScores.value(QStringLiteral("latency")), std::round(1000 * std::pow(2.0, 0.20)));
        TestResult close = atAnchors(false);
        for (Metric &m : close.metrics) {
            if (m.key == QLatin1StringView("cache.levels"))
                m.value = QStringLiteral("L1d 32 KiB · L2 2 MiB · L3 2.5 MiB (too close to the L2: not measured)");
        }
        QVERIFY(!ram::formula_v1::scoresL3(close));
        QCOMPARE(*t.score(close).total, 1000.0);
        // An L3 that is there but wasn't measured: no latency sub-score, no total.
        r = atAnchors(true);
        r.metrics.removeIf([](const Metric &m) { return m.key == QLatin1StringView("latency.l3"); });
        b = t.score(r);
        QVERIFY(!b.total);
        QVERIFY(!b.subScores.contains(QStringLiteral("latency")));
        QCOMPARE(b.subScores.value(QStringLiteral("bandwidth")), 1000.0);
        // AP-5 M2: a part that ended in an Error (a plain-store check failing
        // after the scored bandwidth measurements) has no sub-score, even with
        // all its scored metrics there.
        r = atAnchors(true);
        r.status = Status::Partial;
        r.subResults = {{QStringLiteral("latency"), Status::Ok, {}, {}},
                        {QStringLiteral("bandwidth"), Status::Error, QStringLiteral("Copy bandwidth, plain stores (all threads): ..."), {}},
                        {QStringLiteral("loaded"), Status::Ok, {}, {}}};
        b = t.score(r);
        QVERIFY(!b.total);
        QVERIFY(!b.subScores.contains(QStringLiteral("bandwidth")));
        QCOMPARE(b.subScores.size(), 2);
        // Partial: the sub-scores, no total.
        r = atAnchors(true);
        r.status = Status::Partial;
        b = t.score(r);
        QVERIFY(!b.total);
        QCOMPARE(b.subScores.size(), 3);
    }

    // Task 12: the Detail page of the approved mockup, from its own numbers.
    void detailCompleted()
    {
        tests::RamTest t;
        const DetailLayout d = t.detail(mockupResult());
        QCOMPARE(d.subtitle, QStringLiteral("Score 1,186 · DDR5-6000 · 2 MiB pages · latency on CPU 4"));
        QCOMPARE(d.headlineMetrics, (QStringList{QStringLiteral("latency.dram"), QStringLiteral("bandwidth.read")}));
        QCOMPARE(d.headlineLabels.value(QStringLiteral("latency.dram")), QStringLiteral("DRAM"));  // Zeb D2
        QCOMPARE(d.headlineLabels.value(QStringLiteral("bandwidth.read")), QStringLiteral("Read"));
        // The mockup's blocks, in order (the renderer makes the rows).
        QCOMPARE(titles(d), (QStringList{QStringLiteral("[kpis]"), QStringLiteral("Sub-tests"),
                                         QStringLiteral("Latency by buffer size (CPU 4 · 2 MiB pages)"), QStringLiteral("Latency (CPU 4)"),
                                         QStringLiteral("[note]"), QStringLiteral("Bandwidth (GB/s)"), QStringLiteral("[note]"),
                                         QStringLiteral("Loaded latency (latency against load bandwidth)"),
                                         QStringLiteral("Loaded latency points"), QStringLiteral("[note]"),
                                         QStringLiteral("Memory configuration (SMBIOS)"),
                                         QStringLiteral("Theoretical peak (derived from SMBIOS)"), QStringLiteral("Modules"),
                                         QStringLiteral("[note]")}));
        // KPIs: five, the Memory one on two lines.
        const auto &kpis = std::get<KpisBlock>(d.blocks.first().content);
        QCOMPARE(kpis.items.size(), 5);
        const QStringList labels = {QStringLiteral("RAM score"), QStringLiteral("DRAM latency"), QStringLiteral("Read bandwidth"),
                                    QStringLiteral("Loaded latency"), QStringLiteral("Memory")};
        const QStringList values = {QStringLiteral("1,186"), QStringLiteral("74.2 ns"), QStringLiteral("72.4 GB/s"),
                                    QStringLiteral("138 ns"), QStringLiteral("DDR5-6000")};
        const QStringList under = {QStringLiteral("points"), QStringLiteral("1 GiB buffer · CPU 4"), QStringLiteral("all 32 threads"),
                                   QStringLiteral("at 68.6 GB/s load"), QStringLiteral("32 GB · 2 × 16 GB")};
        for (int i = 0; i < 5; ++i) {
            QCOMPARE(kpis.items[i].label, labels[i]);
            QCOMPARE(kpis.items[i].value.text, values[i]);
            QCOMPARE(kpis.items[i].unitOverride, under[i]);
        }
        // Sub-tests: both bandwidth lines (the mockup), the bold total.
        const TableBlock *groups = tableTitled(d, QStringLiteral("Sub-tests"));
        QVERIFY(groups && groups->fullWidth);
        QCOMPARE(cells(*groups, 0), (QStringList{QStringLiteral("Latency"), QStringLiteral("50%"),
                                                 QStringLiteral("DRAM 74.2 ns · L3 9.6 · L2 2.6 · L1 0.8 ns"), QStringLiteral("1,567")}));
        QCOMPARE(cells(*groups, 1).at(2), QStringLiteral("read 72.4 · write 61.8 · copy 66.5 GB/s (all threads)\n"
                                                         "read 41.0 · write 28.6 · copy 36.2 GB/s (single thread)"));
        QCOMPARE(cells(*groups, 2), (QStringList{QStringLiteral("Loaded latency"), QStringLiteral("15%"),
                                                 QStringLiteral("138 ns at 68.6 GB/s"), QStringLiteral("1,522")}));
        QCOMPARE(cells(*groups, 3), (QStringList{QStringLiteral("RAM score"), QStringLiteral("100%"),
                                                 QStringLiteral("weighted geometric mean"), QStringLiteral("1,186")}));
        QCOMPARE(groups->boldRows, QVector<int>{3});
        // The latency chart: log-log, the OS cache sizes labelled, the four
        // targets dotted, ticks 0.5 ... 200 ns.
        const LineChartBlock *chart = chartTitled(d, QStringLiteral("Latency by buffer size"));
        QVERIFY(chart);
        QCOMPARE(chart->xAxis.scale, ChartAxis::Scale::Log2);
        QCOMPARE(chart->yAxis.scale, ChartAxis::Scale::Log10);
        QCOMPARE(chart->yAxis.tickLabels, (QStringList{QStringLiteral("0.5"), QStringLiteral("1"), QStringLiteral("2"),
                                                       QStringLiteral("5"), QStringLiteral("10"), QStringLiteral("20"),
                                                       QStringLiteral("50"), QStringLiteral("100"), QStringLiteral("200")}));
        QCOMPARE(chart->xAxis.tickLabels.size(), 10);
        QCOMPARE(chart->annotations.size(), 3);
        QCOMPARE(chart->annotations[0].label, QStringLiteral("L1 48 KiB"));
        QCOMPARE(chart->annotations[0].x, 48.0);
        QCOMPARE(chart->annotations[2].label, QStringLiteral("L3 32 MiB"));
        QCOMPARE(chart->markers, (QVector<double>{24, 220, 5836.8}));  // DRAM is the filled last point
        const TableBlock *lat = tableTitled(d, QStringLiteral("Latency (CPU 4)"));
        QVERIFY(lat && !lat->fullWidth);
        QCOMPARE(cells(*lat, 0), (QStringList{QStringLiteral("L1"), QStringLiteral("48 KiB"), QStringLiteral("24 KiB"), QStringLiteral("0.8 ns")}));
        QCOMPARE(cells(*lat, 2), (QStringList{QStringLiteral("L3"), QStringLiteral("32 MiB"), QStringLiteral("5.7 MiB"), QStringLiteral("9.6 ns")}));
        QCOMPARE(cells(*lat, 3), (QStringList{QStringLiteral("DRAM"), QStringLiteral("—"), QStringLiteral("1 GiB"), QStringLiteral("74.2 ns")}));
        // Bandwidth: the x64 plain-store columns, not scored.
        const TableBlock *bw = tableTitled(d, QStringLiteral("Bandwidth (GB/s)"));
        QVERIFY(bw && bw->fullWidth);
        QCOMPARE(bw->columns, (QStringList{QStringLiteral("Threads"), QStringLiteral("Read"), QStringLiteral("Write"), QStringLiteral("Copy"),
                                           QStringLiteral("Write, plain stores (not scored)"), QStringLiteral("Copy, plain stores (not scored)")}));
        QCOMPARE(cells(*bw, 0), (QStringList{QStringLiteral("All threads (32)"), QStringLiteral("72.4"), QStringLiteral("61.8"),
                                             QStringLiteral("66.5"), QStringLiteral("41.3"), QStringLiteral("52.6")}));
        QCOMPARE(cells(*bw, 1).first(), QStringLiteral("Single thread (CPU 4)"));
        // Loaded latency: the six points in level order, full load bold.
        const TableBlock *points = tableTitled(d, QStringLiteral("Loaded latency points"));
        QVERIFY(points);
        QCOMPARE(points->columns, (QStringList{QStringLiteral("Load level"), QStringLiteral("GB/s"), QStringLiteral("ns")}));
        QStringList levels;
        for (int i = 0; i < points->rows.size(); ++i)
            levels << cells(*points, i).first();
        QCOMPARE(levels, (QStringList{QStringLiteral("Idle (no load)"), QStringLiteral("6,000 ns delay"), QStringLiteral("2,000 ns delay"),
                                      QStringLiteral("750 ns delay"), QStringLiteral("250 ns delay"), QStringLiteral("Full load (scored)")}));
        QCOMPARE(cells(*points, 0), (QStringList{QStringLiteral("Idle (no load)"), QStringLiteral("0"), QStringLiteral("74.2")}));
        QCOMPARE(cells(*points, 5), (QStringList{QStringLiteral("Full load (scored)"), QStringLiteral("68.6"), QStringLiteral("138")}));
        QCOMPARE(points->boldRows, QVector<int>{5});
        const LineChartBlock *loadedChart = chartTitled(d, QStringLiteral("Loaded latency (latency"));
        QVERIFY(loadedChart);
        QVERIFY(loadedChart->sortByX);  // drawn by bandwidth (Dave, 2026-10-20)
        QCOMPARE(*loadedChart->filledAt, 68.6);  // the filled dot on full load (AP-5 L1)
        QCOMPARE(loadedChart->xAxis.tickLabels, (QStringList{QStringLiteral("0 GB/s"), QStringLiteral("20 GB/s"), QStringLiteral("40 GB/s"),
                                                             QStringLiteral("60 GB/s"), QStringLiteral("80 GB/s")}));
        QCOMPARE(loadedChart->annotations.size(), 2);
        QCOMPARE(loadedChart->annotations[0].label, QStringLiteral("Idle 74.2 ns"));
        QCOMPARE(loadedChart->annotations[1].label, QStringLiteral("Full load 138 ns"));
        // The memory configuration: readout | derived peak, then the modules
        // with the JEP106 maker.
        const TableBlock *config = tableTitled(d, QStringLiteral("Memory configuration (SMBIOS)"));
        const TableBlock *peak = tableTitled(d, QStringLiteral("Theoretical peak (derived from SMBIOS)"));
        QVERIFY(config && peak && !config->fullWidth && !peak->fullWidth);
        QCOMPARE(config->rows.size(), 8);
        QCOMPARE(cells(*config, 1), (QStringList{QStringLiteral("Configured speed"), QStringLiteral("6,000 MT/s")}));
        QCOMPARE(cells(*config, 7), (QStringList{QStringLiteral("Channels (estimate)"), QStringLiteral("2")}));
        QCOMPARE(cells(*peak, 0), (QStringList{QStringLiteral("Theoretical peak"), QStringLiteral("96.0 GB/s")}));
        QCOMPARE(cells(*peak, 1), (QStringList{QStringLiteral("Derived as"), QStringLiteral("6,000 MT/s × 128-bit ÷ 8")}));
        QCOMPARE(cells(*peak, 3), (QStringList{QStringLiteral("Read efficiency"), QStringLiteral("75%")}));
        const TableBlock *modules = tableTitled(d, QStringLiteral("Modules"));
        QVERIFY(modules && modules->fullWidth);
        QCOMPARE(modules->columns.at(5), QStringLiteral("Maker"));
        QCOMPARE(cells(*modules, 0), (QStringList{QStringLiteral("DIMMA2 (P0 CHANNEL A)"), QStringLiteral("16 GB"), QStringLiteral("DDR5"),
                                                  QStringLiteral("6,000 MT/s"), QStringLiteral("2R"), QStringLiteral("G.Skill"),
                                                  QStringLiteral("EX6000C30-16G")}));
        // The notes, word for word as approved; neutral (amber as built).
        Tone first = Tone::Warn;
        const QStringList n = notes(d, &first);
        QCOMPARE(first, Tone::Neutral);
        QCOMPARE(n, (QStringList{
                        QStringLiteral("Cache sizes as the OS reports them for CPU 4: L1d 48 KiB · L2 1 MiB · L3 32 MiB (shared by CPUs "
                                       "0–7, 16–23). Each level is measured in the middle of its plateau (the open dots); the chart's "
                                       "y axis is logarithmic."),
                        QStringLiteral("AVX2 loads. Write and copy are scored with non-temporal stores; the plain-store columns are "
                                       "shown for reference. Copy counts bytes read + written."),
                        QStringLiteral("The DRAM latency chase runs on CPU 4 while 30 load threads on the other CPUs (not CPU 4's SMT "
                                       "sibling) stream reads, pausing after every 4 KiB for the level's delay. Only full load is "
                                       "scored."),
                        QStringLiteral("As reported by the firmware (SMBIOS). SynBench doesn't read SPD, so timings aren't shown.")}));
    }

    // The mockup's variants: no L3 (Snapdragon X), ARM64 bandwidth, 4 KiB
    // and mixed pages, no SMBIOS readout, emulation, P9's cap hit.
    void detailVariants()
    {
        tests::RamTest t;
        TestResult r = mockupResult();
        r.metrics.removeIf([](const Metric &m) {
            return m.key == QLatin1StringView("latency.l3") || m.key == QLatin1StringView("latency.l3.at")
                   || m.key.startsWith(QLatin1StringView("config.")) || m.key == QLatin1StringView("speed")
                   || m.key == QLatin1StringView("channels") || m.key.contains(QLatin1StringView(".plain"));
        });
        for (Metric &m : r.metrics) {
            if (m.key == QLatin1StringView("cache.levels"))
                m.value = QStringLiteral("L1d 96 KiB · L2 12 MiB (shared by CPUs 0–3)");
            if (m.key == QLatin1StringView("bandwidth.path"))
                m.value = QStringLiteral("NEON loads and plain stores");
            if (m.key == QLatin1StringView("pages"))
                m.value = QStringLiteral("4 KiB pages (no SeLockMemoryPrivilege)");
        }
        r.metrics.append({QStringLiteral("config.source"), {}, QString(),
                          QStringLiteral("not available: the SMBIOS helper wasn't authorised and udev has no memory data"), {},
                          Direction::Neutral, false});
        r.metrics.append({QStringLiteral("emulated"), {}, QString(), QStringLiteral("x64 build under emulation on ARM64"), {},
                          Direction::Neutral, false});
        r.metrics.append({QStringLiteral("bandwidth.limited"), {}, QString(),
                          QStringLiteral("Bandwidth buffers were limited to 25% of memory (64 MiB per thread, below the 96 MiB L3), "
                                         "so bandwidth may partly come from cache."),
                          {}, Direction::Neutral, false});
        const DetailLayout d = t.detail(r);
        QCOMPARE(d.subtitle, QStringLiteral("Score 1,186 · 4 KiB pages · latency on CPU 4"));
        const TableBlock *lat = tableTitled(d, QStringLiteral("Latency (CPU 4)"));
        QCOMPARE(cells(*lat, 2), (QStringList{QStringLiteral("L3"), QStringLiteral("none"), QStringLiteral("—"), QStringLiteral("—")}));
        QCOMPARE(cells(*tableTitled(d, QStringLiteral("Sub-tests")), 0).at(2), QStringLiteral("DRAM 74.2 ns · L2 2.6 · L1 0.8 ns"));
        QCOMPARE(chartTitled(d, QStringLiteral("Latency by buffer size"))->markers.size(), 2);
        QCOMPARE(tableTitled(d, QStringLiteral("Bandwidth (GB/s)"))->columns.size(), 4);  // no plain-store columns
        QVERIFY(!tableTitled(d, QStringLiteral("Memory configuration (SMBIOS)")));
        const QStringList n = notes(d);
        QVERIFY(n.contains(QStringLiteral("NEON loads and plain stores (ARM has no guaranteed non-temporal store). Copy counts bytes "
                                          "read + written.")));
        QVERIFY(n.contains(QStringLiteral("Memory details not available: the SMBIOS helper wasn't authorised and udev has no memory "
                                          "data. The test still ran and is scored.")));
        QVERIFY(n.contains(QStringLiteral("This run used 4 KiB pages: DRAM and loaded latency include TLB misses, so they read higher "
                                          "than on a huge-page run. The score is not adjusted.")));
        QVERIFY(n.contains(QStringLiteral("This x64 build ran under emulation (Prism): latency is only roughly right. Run the ARM64 "
                                          "build for real numbers.")));
        QVERIFY(n.contains(QStringLiteral("Bandwidth buffers were limited to 25% of memory (64 MiB per thread, below the 96 MiB L3), "
                                          "so bandwidth may partly come from cache.")));
        // Mixed pages.
        TestResult mixed = mockupResult();
        for (Metric &m : mixed.metrics) {
            if (m.key == QLatin1StringView("pages"))
                m.value = QStringLiteral("mixed: 41% 2 MiB pages");
            if (m.key == QLatin1StringView("pages.huge"))
                m.value = 41.3;
        }
        const DetailLayout md = t.detail(mixed);
        QVERIFY(md.subtitle.contains(QLatin1StringView("41% 2 MiB pages")));
        QVERIFY(notes(md).contains(QStringLiteral("Only 41% of the latency buffer got 2 MiB pages, so DRAM and loaded latency may read "
                                                  "higher than on a full huge-page run. The score is not adjusted.")));
        // A latency that wasn't measured is a dash in its table, never a 0
        // (Zeb, checkpoint 1); the chart only has what was measured.
        TestResult missing = mockupResult();
        missing.metrics.removeIf([](const Metric &m) { return m.key == QLatin1StringView("latency.l2"); });
        QCOMPARE(cells(*tableTitled(t.detail(missing), QStringLiteral("Latency (CPU 4)")), 1).at(3), QStringLiteral("—"));
    }

    // Partial: no total, the parts' problems in a note; Stopped: no
    // Sub-score column, the parts that didn't finish say so.
    void detailPartialAndStopped()
    {
        tests::RamTest t;
        TestResult r = mockupResult();
        r.status = Status::Partial;
        r.score.reset();
        r.subResults[2] = {QStringLiteral("loaded"), Status::Error, QStringLiteral("chase under load: the pointer chase left its buffer"), {}};
        r.metrics.removeIf([](const Metric &m) { return m.key.startsWith(QLatin1StringView("loaded.")); });
        r.series.removeLast();
        DetailLayout d = t.detail(r);
        QCOMPARE(d.subtitle, QStringLiteral("No RAM score · DDR5-6000 · 2 MiB pages · latency on CPU 4"));
        QCOMPARE(std::get<KpisBlock>(d.blocks.first().content).items.size(), 3);  // no score, no loaded latency
        const TableBlock *groups = tableTitled(d, QStringLiteral("Sub-tests"));
        QCOMPARE(cells(*groups, 2), (QStringList{QStringLiteral("Loaded latency"), QStringLiteral("15%"), QStringLiteral("Error"), QStringLiteral("—")}));
        QCOMPARE(cells(*groups, 3).at(2), QStringLiteral("no score: not every part ran"));
        QVERIFY(!tableTitled(d, QStringLiteral("Loaded latency points")));
        QVERIFY(notes(d).contains(QStringLiteral("No RAM score: not every part ran. Loaded latency: chase under load: the pointer "
                                                 "chase left its buffer.")));

        TestResult s = mockupResult();
        s.status = Status::Cancelled;
        s.score.reset();
        s.subResults = {{QStringLiteral("latency"), Status::Ok, {}, {}}, {QStringLiteral("bandwidth"), Status::Cancelled, {}, {}}};
        s.metrics.removeIf([](const Metric &m) { return m.key.startsWith(QLatin1StringView("loaded.")) || m.key.startsWith(QLatin1StringView("bandwidth.")); });
        s.series.removeLast();
        d = t.detail(s);
        QVERIFY(d.subtitle.startsWith(QStringLiteral("Stopped · ")));
        groups = tableTitled(d, QStringLiteral("Sub-tests"));
        QCOMPARE(groups->columns.size(), 3);
        QCOMPARE(groups->rows.size(), 3);  // no total row
        QCOMPARE(cells(*groups, 1).at(2), QStringLiteral("Stopped"));
        QCOMPARE(cells(*groups, 2).at(2), QStringLiteral("Not run"));
        QVERIFY(!tableTitled(d, QStringLiteral("Bandwidth (GB/s)")));
        QVERIFY(notes(d).contains(QStringLiteral("Stopped before the end: the parts that finished are shown, without scores.")));
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
        // The load leaves the chased core out; on a one-CPU runner the test
        // hook puts it on that CPU too (SYNBENCH_RAM_LOAD_SHARES_CORE).
        const bool shared = ctx.logged(QStringLiteral("SYNBENCH_RAM_LOAD_SHARES_CORE"));
        QVERIFY(numberOf(r, "loaded.threads") >= 1);
        QVERIFY(numberOf(r, "loaded.threads") < numberOf(r, "threads") || (shared && numberOf(r, "threads") == 1));
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
        // The parts, for the progress pill (Dave, 2026-10-20; the mockup's
        // "RAM · All-thread bandwidth").
        QCOMPARE(ctx.parts(), (QStringList{QStringLiteral("Preparing"), QStringLiteral("Latency"), QStringLiteral("Single-thread bandwidth"),
                                           QStringLiteral("All-thread bandwidth"), QStringLiteral("Loaded latency")}));
        for (const char *line : {"Starting RAM: ", "Pages: ", "Latency on ", "Latency curve: ", "Latency L1 ", "Single thread on ",
                                 "Single thread: read ", "All threads: ", "Read ", "Loaded latency on ", "Full load: "})
            QVERIFY2(ctx.logged(QLatin1StringView(line)), line);
        QCOMPARE(ram::LoadGenerator::liveThreads(), 0);
        QCOMPARE(cpu::WorkerPool::liveThreads(), 0);
        // formula v1 scores it (provisional anchors: the number itself means little).
        const ScoreBreakdown b = t.score(r);
        QVERIFY(b.total && *b.total > 0);
        QCOMPARE(b.subScores.size(), 3);
    }

    void stopInEachPart_data()
    {
        QTest::addColumn<QString>("stage");
        for (const char *stage : {"Preparing buffers", "Latency curve · 4 KiB", "Latency curve · 1 GiB", "Latency L2",
                                  "Latency curve · warming up", "Preparing bandwidth buffers", "Single-thread bandwidth · warming up", "Read", "Copy",
                                  "All-thread bandwidth: Copy", "Loaded latency · warming up", "Loaded latency · preparing",
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

    // At real time, Stop 300 ms into the latency warm-up (on a core with a
    // top clock that may already be warm: then in the grid; without one,
    // inside the 2 s cap): back within 250 ms either way.
    void stopDuringWarmUp()
    {
        tests::setStubTimeScale(1.0);
        tests::RamTest t;
        FakeRunContext ctx;
        ctx.cancelOn(QStringLiteral("Latency curve · warming up"), std::chrono::milliseconds(300));
        const TestResult r = t.run(ctx, {});
        const auto took = Clock::now() - ctx.cancelAt();
        QVERIFY2(ctx.cancelAt() != Clock::time_point{}, "Stop never came");
        const double ms = std::chrono::duration<double, std::milli>(took).count();
        qInfo("Stop 300 ms into the warm-up: %.1f ms", ms);
        QCOMPARE(r.status, Status::Cancelled);
        QVERIFY2(ms < 250, qPrintable(QStringLiteral("Stop took %1 ms").arg(ms, 0, 'f', 1)));
        QCOMPARE(ram::LoadGenerator::liveThreads(), 0);
        QCOMPARE(cpu::WorkerPool::liveThreads(), 0);
    }

    // A part whose measurement fails is an Error with the reason; the others
    // run; the test is Partial.
    void failingPartIsPartial_data()
    {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<QString>("part");
        QTest::newRow("latency") << QStringLiteral("Latency DRAM") << QStringLiteral("latency");
        QTest::newRow("bandwidth") << QStringLiteral("Copy") << QStringLiteral("bandwidth");
        // AP-5 L2: the all-thread pass, which shares its stages with the
        // single-thread one, is reached through its part.
        QTest::newRow("bandwidth, all threads") << QStringLiteral("All-thread bandwidth: Copy") << QStringLiteral("bandwidth");
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
        if (stage.startsWith(QLatin1StringView("All-thread"))) {
            QVERIFY(r.metric(QStringLiteral("bandwidth.st.copy")));  // the single-thread pass finished
            QVERIFY(r.metric(QStringLiteral("bandwidth.write")));
            QVERIFY(!r.metric(QStringLiteral("bandwidth.copy")));
        }
        if (part == QLatin1StringView("latency")) {
            QVERIFY(r.metric(QStringLiteral("latency.l1")));
            QVERIFY(!r.metric(QStringLiteral("latency.dram")));
            // AP-5 M1: the targets measured before it are on the curve, in order.
            const Series *curve = nullptr;
            for (const Series &s : r.series)
                curve = s.key == QLatin1StringView("latency_curve") ? &s : curve;
            QVERIFY(curve);
            for (qsizetype i = 1; i < curve->points.size(); ++i)
                QVERIFY2(curve->points[i].x() > curve->points[i - 1].x(), qPrintable(QString::number(curve->points[i].x())));
        }
    }
};

QTEST_GUILESS_MAIN(TestRamTest)
#include "tst_ram_test.moc"
