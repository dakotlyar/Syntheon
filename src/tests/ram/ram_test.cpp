// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/ram_test.h"

#include "core/units.h"
#include "isa/cache_topology.h"
#include "isa/core_ranking.h"
#include "isa/cpu_features.h"
#include "tests/common/stub_clock.h"
#include "tests/cpu/cpu_context.h"
#include "tests/cpu/engine/platform_threads.h"
#include "tests/cpu/engine/time_scale.h"
#include "tests/cpu/engine/worker_pool.h"
#include "tests/cpu/telemetry/telemetry.h"
#include "tests/ram/config/readout.h"
#include "tests/ram/formula_v1.h"
#include "tests/ram/kernels/bw_kernels.h"
#include "tests/ram/kernels/chase.h"
#include "tests/ram/kernels/cycle_builder.h"
#include "tests/ram/loaded/load_generator.h"
#include "tests/ram/loaded/loaded_latency.h"
#include "tests/ram/memory/huge_buffer.h"
#include "tests/ram/sweep/grid.h"
#include "tests/ram/sweep/knees.h"
#include "tests/ram/sweep/targets.h"

#include <QDir>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace sb::tests {

using namespace std::chrono_literals;
using cpu::Measurement;
using cpu::WindowSpec;
using cpu::WorkerPool;

namespace {

// §3: the windows at real time (the dev time scale shortens them).
constexpr std::chrono::milliseconds kGridRep{60};
constexpr std::chrono::milliseconds kGridWarmMin{10}, kGridWarmMax{300};
constexpr std::chrono::milliseconds kTargetRep{500};
constexpr std::chrono::milliseconds kTargetWarmMin{100}, kTargetWarmMax{1000};
constexpr int kReps = 5;
constexpr WindowSpec kBandwidth{1000ms, 1500ms, kReps};
constexpr WindowSpec kLoaded{1000ms, 1200ms, kReps};
// §4.3: the single-thread core's idle sample, once per sub-test.
constexpr std::chrono::milliseconds kCoreSample{250};
// §4.5: a background load from this logs a warning (the CPU test's rule).
constexpr double kBackgroundWarnPercent = 10;
// D18: the free memory wanted on top of a buffer.
constexpr std::uint64_t kFreeMargin = 512ull << 20;
// §5.1: the first grid point has no previous point to size its units from;
// 1 ns (an L1 hop is 0.7-1.5 ns) keeps its units near 2 ms.
constexpr double kFirstGridEstimateNs = 1.0;
// §5.4: the loaded chase's unit size when the idle DRAM latency is missing.
constexpr double kDefaultDramEstimateNs = 100;
// §9: an all-thread read above this x the SMBIOS peak logs a warning.
constexpr double kPeakWarnFactor = 1.3;

constexpr std::uint64_t KiB = 1024, MiB = 1024 * KiB, GiB = 1024 * MiB;

#if defined(__x86_64__) || defined(_M_X64)
constexpr bool kX64 = true;
#else
constexpr bool kX64 = false;
#endif
#if defined(_WIN32)
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

// The sub-tests (ids stable forever, P1), in run order.
const QString kLatency = QStringLiteral("latency");
const QString kBandwidthId = QStringLiteral("bandwidth");
const QString kLoadedId = QStringLiteral("loaded");

double seconds(std::chrono::milliseconds ms)
{
    return double(ms.count()) / 1000.0;
}

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

QString coreName(const isa::CoreChoice &c)
{
    if (c.group > 0)
        return QStringLiteral("%1:%2").arg(c.group).arg(c.cpu);
    return QString::number(c.cpu);
}

QString keyName(int key)
{
    // isa::cpuKey: windowsKey(group, index); the CPU number on Linux.
    return key >= 64 ? QStringLiteral("%1:%2").arg(key / 64).arg(key % 64) : QString::number(key);
}

// "0–7, 16–23": ascending keys as ranges.
QString cpuRanges(std::vector<int> keys)
{
    std::sort(keys.begin(), keys.end());
    QStringList parts;
    for (std::size_t i = 0; i < keys.size();) {
        std::size_t j = i;
        while (j + 1 < keys.size() && keys[j + 1] == keys[j] + 1 && keys[j + 1] / 64 == keys[i] / 64)
            ++j;
        parts << (j > i ? keyName(keys[i]) + QStringLiteral("–") + keyName(keys[j]) : keyName(keys[i]));
        i = j + 1;
    }
    return parts.join(QStringLiteral(", "));
}

// "24 KiB", "5.7 MiB", "1 GiB" (one decimal unless whole).
QString sizeText(std::uint64_t bytes)
{
    const auto in = [&](std::uint64_t unit, const char *name) {
        const double v = double(bytes) / double(unit);
        return bytes % unit == 0 || v >= 100 ? QStringLiteral("%1 %2").arg(std::llround(v)).arg(QLatin1StringView(name))
                                            : QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QLatin1StringView(name));
    };
    if (bytes >= GiB)
        return in(GiB, "GiB");
    if (bytes >= MiB)
        return in(MiB, "MiB");
    return in(KiB, "KiB");
}

// "1.5 GiB", for the free-memory messages.
QString gibText(std::uint64_t bytes)
{
    return QStringLiteral("%1 GiB").arg(double(bytes) / double(GiB), 0, 'f', 1);
}

// Latency as the mockup prints it: "0.8", "74.2", "138".
QString nsText(double ns)
{
    return ns >= 100 ? QString::number(std::llround(ns)) : QString::number(ns, 'f', 1);
}

QString gbsText(double gbs)
{
    return QString::number(gbs, 'f', 1);
}

Metric number(const QString &key, const QString &sub, const QString &label, double value, const QString &unit,
              Direction direction, bool headline = false)
{
    return {key, sub, label, value, unit, direction, headline};
}

Metric text(const QString &key, const QString &sub, const QString &label, const QString &value)
{
    return {key, sub, label, value, {}, Direction::Neutral, false};
}

// §4.2: NUMA nodes (Linux: /sys/devices/system/node/node*; Windows:
// GetNumaHighestNodeNumber). At least 1.
int numaNodes()
{
#if defined(_WIN32)
    ULONG highest = 0;
    return GetNumaHighestNodeNumber(&highest) ? int(highest) + 1 : 1;
#elif defined(__linux__)
    const QStringList nodes = QDir(QStringLiteral("/sys/devices/system/node"))
                                  .entryList({QStringLiteral("node*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    int n = 0;
    for (const QString &d : nodes) {
        bool ok = false;
        d.mid(4).toInt(&ok);
        n += ok ? 1 : 0;
    }
    return std::max(1, n);
#else
    return 1;
#endif
}

// The snapshot's physical cores (0: unknown).
int physicalCores(const SystemSnapshot &s)
{
    return s.data.value(QStringLiteral("cpu")).toObject().value(QStringLiteral("cores")).toInt();
}

std::uint64_t physicalMemory(const SystemSnapshot &s)
{
    return std::uint64_t(s.data.value(QStringLiteral("memory")).toObject().value(QStringLiteral("total_bytes")).toDouble());
}

// Every allowed CPU's topology, for the bandwidth plan's last-level caches.
std::vector<isa::CacheTopology> allTopologies(const std::vector<isa::CoreChoice> &cpus)
{
    std::vector<isa::CacheTopology> out;
    out.reserve(cpus.size());
    for (const isa::CoreChoice &c : cpus)
        out.push_back(isa::cacheTopologyFor(c));
    return out;
}

// Runs `fn` on a thread of its own pinned to `core` (unranked: where the OS
// puts it), and waits for it: the cycle builds and the latency buffer's first
// touch happen on the chased core (§5.1, §6.1).
template <typename F>
void onCore(const isa::CoreChoice &core, F &&fn)
{
    std::exception_ptr error;
    std::thread t([&] {
        if (core.ranked)
            cpu::pinCurrentThread(core);
        try {
            fn();
        } catch (...) {
            error = std::current_exception();
        }
    });
    t.join();
    if (error)
        std::rethrow_exception(error);
}

// cache.levels (§2): "L1d 48 KiB · L2 1 MiB · L3 32 MiB (shared by CPUs
// 0–7, 16–23)". A level whose target is absent under §5.2's range rule says
// "(too close to the L2: not measured)": that is how score() tells it from a
// level that is there (formula_v1::scoresL3).
QString cacheLevelsText(const isa::CacheTopology &t, const ram::LatencyTargets &targets)
{
    QStringList parts;
    for (const isa::CacheLevel &c : t.levels) {
        QString name = QStringLiteral("L%1").arg(c.level);
        if (c.level == 1 && c.type == isa::CacheLevel::Type::Data)
            name += QLatin1Char('d');
        QString part = name + QLatin1Char(' ') + sizeText(c.sizeBytes);
        if (c.level >= 2 && c.level <= 3 && !targets.level(c.level).present())
            part += QStringLiteral(" (too close to the L%1: not measured)").arg(c.level - 1);
        if (&c == t.lastLevel() && c.sharedWith.size() > 1)
            part += QStringLiteral(" (shared by CPUs %1)").arg(cpuRanges(c.sharedWith));
        parts << part;
    }
    return parts.join(QStringLiteral(" · "));
}

// The fallback's cache.levels: "L1 48 KiB · L2 1 MiB (detected from the curve)".
QString curveLevelsText(const ram::LatencyTargets &t)
{
    QStringList parts;
    for (int k = 1; k <= 3; ++k) {
        const ram::LevelTarget &l = t.level(k);
        if (l.cacheBytes > 0)
            parts << QStringLiteral("L%1 %2").arg(k).arg(sizeText(l.cacheBytes))
                         + (l.present() || k == 1 ? QString() : QStringLiteral(" (too close to the L%1: not measured)").arg(k - 1));
    }
    return (parts.isEmpty() ? QStringLiteral("no cache levels") : parts.join(QStringLiteral(" · ")))
           + QStringLiteral(" (detected from the curve)");
}

QString opName(const ram::BwVariant &v)
{
    return v.op == ram::BwOp::Read ? QStringLiteral("Read") : v.op == ram::BwOp::Write ? QStringLiteral("Write") : QStringLiteral("Copy");
}

// The stage of one bandwidth measurement: "Write, plain stores" (all
// threads, the mockup's stage line), "Single-thread write, plain stores".
QString variantStage(const ram::BwVariant &v, bool single)
{
    QString op = opName(v);
    if (!v.scored)
        op += QStringLiteral(", plain stores");
    return single ? QStringLiteral("Single-thread ") + op.toLower() : op;
}

// The metric label: "Write bandwidth, plain stores (all threads)".
QString variantLabel(const ram::BwVariant &v, bool single)
{
    QString op = opName(v) + QStringLiteral(" bandwidth");
    if (!v.scored)
        op += QStringLiteral(", plain stores");
    return QStringLiteral("%1 (%2)").arg(op, single ? QStringLiteral("single thread") : QStringLiteral("all threads"));
}

struct CorePick {
    isa::CoreChoice core;
    std::vector<int> smtKeys;  // the chased core's physical core (itself and its SMT siblings)
    QString text;              // latency.core's wording
};

// One RAM test run: §3's sequence, with progress and the log.
class RamRun
{
public:
    explicit RamRun(RunContext &ctx)
        : m_ctx(ctx), m_allowed(isa::allowedLogicalCpus()),
          m_threads(m_allowed.empty() ? cpu::logicalCpuCount() : int(m_allowed.size())),
          m_path(ram::chooseBandwidthPath(isa::detect())), m_variants(ram::bandwidthVariants(m_path)),
          m_readout(ram::memoryReadout(ctx.system(), kWindows))
    {
    }

    TestResult run();

private:
    bool cancelled() const { return m_ctx.cancelled(); }
    std::function<bool()> cancelFn() const
    {
        return [this] { return cancelled(); };
    }
    bool stopRequested()
    {
        if (cancelled())
            m_stopped = true;
        return m_stopped;
    }

    // Progress against §3's budget (at the time scale), never backwards.
    void report(double at, const QString &stage);
    std::function<void(int)> windowProgress(const QString &stage, const WindowSpec &spec, double base);
    static double budget(const WindowSpec &s) { return seconds(s.warmup) + seconds(s.rep) * s.reps; }
    double gridPointBudget() const;
    double targetBudget() const;
    void planBudget();

    void groupBegin(const QString &id);
    void groupFail(const QString &id, Status status, const QString &reason);
    void groupEnd(const QString &id);
    // A measurement that didn't give a result: the group is an Error (with
    // the reason), or the run was stopped. True if so.
    bool failed(const QString &group, const Measurement &m, const QString &what);
    void forcedFailure(Measurement &m, const QString &stage) const;
    std::optional<QString> shortOfMemory(std::uint64_t bytes) const;

    CorePick pickCore();
    std::unique_ptr<WorkerPool> chasePool(const isa::CoreChoice &core) const;
    // Builds the cycle over the latency buffer's first `bytes` on `core` and
    // measures it (ns per hop). Nullopt if the group failed or the run stopped.
    std::optional<double> chase(WorkerPool &pool, const isa::CoreChoice &core, std::uint64_t bytes, double estimateNs,
                                const WindowSpec &spec, const QString &stage, double budgetAt, double *spread = nullptr);

    void prepare();
    void latency();
    bool latencyGrid();
    void latencyTargets();
    bool prepareBandwidth();
    void bandwidth();
    void bandwidthStep(WorkerPool &pool, bool single);
    void loaded();
    void finish(TestResult &r);

    RunContext &m_ctx;
    const std::vector<isa::CoreChoice> m_allowed;  // every logical CPU, for pinEach and the load (P11, P12)
    const int m_threads;
    const ram::BwPath m_path;
    const std::vector<ram::BwVariant> m_variants;
    const ram::MemoryReadout m_readout;
    const std::chrono::milliseconds m_sampleFor = cpu::scaledWindow(kCoreSample);

    double m_budgetTotal = 1, m_budgetDone = 0, m_lastFraction = 0;

    TestResult m_r;
    bool m_stopped = false;
    std::map<QString, Status> m_groupStatus;
    std::map<QString, QString> m_groupReason;
    double m_worstSpread = 0;

    // The latency part (§5.1, §5.2).
    CorePick m_core;
    isa::CacheTopology m_topology;
    ram::LatencyTargets m_targets;
    std::uint64_t m_dramBytes = ram::kDramTargetMin;
    std::size_t m_line = 64;
    std::vector<std::uint64_t> m_grid;
    std::vector<ram::CurvePoint> m_curve;
    std::map<int, double> m_targetNs;  // level 1-3, 4 = DRAM -> ns
    std::optional<double> m_dramNs;
    QString m_latencyProblem;  // why there is no latency buffer
    ram::HugeBuffer m_latencyBuffer;

    // The bandwidth part (§5.3).
    ram::BandwidthPlan m_plan;
    ram::HugeBuffer m_bwBuffer;
    std::vector<std::unique_ptr<ram::BandwidthSlice>> m_slices;
    QString m_bandwidthProblem;
    std::map<std::string, double> m_st, m_mt;  // variant key -> GB/s
};

void RamRun::report(double at, const QString &stage)
{
    m_lastFraction = std::clamp(std::max(m_lastFraction, at / m_budgetTotal), 0.0, 0.999);
    m_ctx.progress(m_lastFraction, stage);
}

std::function<void(int)> RamRun::windowProgress(const QString &stage, const WindowSpec &spec, double base)
{
    return [this, stage, spec, base](int w) {
        const double at = base + (w == 0 ? 0 : seconds(spec.warmup) + seconds(spec.rep) * (w - 1));
        report(at, w == 0 ? stage + QStringLiteral(" · warm-up") : stage + QStringLiteral(" · rep %1 of %2").arg(w).arg(spec.reps));
    };
}

double RamRun::gridPointBudget() const
{
    // A typical warm-up (one lap: 10-300 ms) and the cycle build.
    return seconds(cpu::scaledWindow(100ms)) + kReps * seconds(cpu::scaledWindow(kGridRep)) + 0.05;
}

double RamRun::targetBudget() const
{
    return seconds(cpu::scaledWindow(500ms)) + kReps * seconds(cpu::scaledWindow(kTargetRep)) + 0.2;
}

void RamRun::planBudget()
{
    // §3 at the time scale; the first touches and cycle builds don't scale
    // (a few seconds), so they are counted at real time.
    const double sample = seconds(m_sampleFor);
    double total = 2.5;
    total += double(m_grid.size()) * gridPointBudget() + 4 * targetBudget();
    const double bw = budget(cpu::scaled(kBandwidth));
    total += sample + 1.5 + double(m_variants.size()) * bw;  // single thread (+ the slices' first touch)
    total += double(m_variants.size()) * bw;                  // all threads
    total += sample + 0.5 + double(std::size(ram::kLoadDelaysNs)) * budget(cpu::scaled(kLoaded));
    m_budgetTotal = total;
}

void RamRun::groupBegin(const QString &id)
{
    m_ctx.subTestBegin(id);
    m_groupStatus[id] = Status::Ok;
}

void RamRun::groupFail(const QString &id, Status status, const QString &reason)
{
    if (m_groupStatus[id] == Status::Ok) {
        m_groupStatus[id] = status;
        m_groupReason[id] = reason;
    }
}

void RamRun::groupEnd(const QString &id)
{
    if (m_stopped && m_groupStatus[id] == Status::Ok)
        m_groupStatus[id] = Status::Cancelled;
    m_ctx.subTestEnd(id, m_groupStatus[id]);
}

bool RamRun::failed(const QString &group, const Measurement &m, const QString &what)
{
    if (m.ok())
        return false;
    if (m.outcome == Measurement::Outcome::Cancelled) {
        m_stopped = true;
        return true;
    }
    const QString reason = QStringLiteral("%1: %2").arg(what, qs(m.error));
    m_ctx.log(LogLevel::Error, reason);
    groupFail(group, Status::Error, reason);
    return true;
}

void RamRun::forcedFailure(Measurement &m, const QString &stage) const
{
#if defined(SYNBENCH_DEV_HOOKS)
    // Tests: SYNBENCH_RAM_FAIL=<stage prefix> makes that measurement a wrong result.
    const QString fail = qEnvironmentVariable("SYNBENCH_RAM_FAIL");
    if (m.ok() && !fail.isEmpty() && stage.startsWith(fail)) {
        m.outcome = Measurement::Outcome::WrongResult;
        m.error = "forced wrong result (SYNBENCH_RAM_FAIL)";
    }
#else
    (void)m;
    (void)stage;
#endif
}

std::optional<QString> RamRun::shortOfMemory(std::uint64_t bytes) const
{
    const auto free = cpu::availableMemoryBytes();
    if (!free || *free >= bytes + kFreeMargin)
        return std::nullopt;
    return QStringLiteral("needs %1 of free memory (%2 available)").arg(gibText(bytes + kFreeMargin), gibText(*free));
}

CorePick RamRun::pickCore()
{
    CorePick p;
    const std::vector<isa::CoreCandidate> c = isa::topCandidatesOnOneLastLevelCache();
    if (c.empty()) {
        // §4.3: one thread can't measure the caches unpinned (it could move
        // between L2s), so the first allowed CPU.
        if (!m_allowed.empty()) {
            p.core = m_allowed.front();
            p.smtKeys = {isa::cpuKey(p.core)};
            p.text = QStringLiteral("CPU %1 (no core ranking; first allowed CPU)").arg(coreName(p.core));
        } else {
            p.text = QStringLiteral("unpinned (no core ranking)");
        }
        return p;
    }
    if (c.size() == 1) {
        p.core = c.front().core;
        p.smtKeys = c.front().smtKeys;
        p.text = QStringLiteral("CPU %1 (top-ranked core)").arg(coreName(p.core));
        return p;
    }
    // Decision D's idle sample, once per sub-test (§4.3), polling Stop.
    const isa::IdleSample before = isa::sampleIdle();
    const auto end = std::chrono::steady_clock::now() + m_sampleFor;
    while (!cancelled()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= end)
            break;
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(50ms, end - now));
    }
    const isa::IdleSample after = isa::sampleIdle();
    p.core = isa::quietestCandidate(c, before, after);
    p.smtKeys = c.front().smtKeys;
    QStringList names;
    for (const isa::CoreCandidate &k : c) {
        names << coreName(k.core);
        if (k.core.cpu == p.core.cpu && k.core.group == p.core.group)
            p.smtKeys = k.smtKeys;  // the chosen candidate's siblings
    }
    p.text = QStringLiteral("CPU %1 (top-ranked core, quietest of CPUs %2)").arg(coreName(p.core), names.join(QStringLiteral(", ")));
    return p;
}

std::unique_ptr<WorkerPool> RamRun::chasePool(const isa::CoreChoice &core) const
{
    WorkerPool::Options o;
    o.threads = 1;
    o.pinTo = core;  // no pickFrom: the core is picked once per sub-test (§4.3, §5.4)
    return std::make_unique<WorkerPool>(o);
}

std::optional<double> RamRun::chase(WorkerPool &pool, const isa::CoreChoice &core, std::uint64_t bytes, double estimateNs,
                                    const WindowSpec &spec, const QString &stage, double budgetAt, double *spread)
{
    ram::CycleBuild built = ram::CycleBuild::Ok;
    onCore(core, [&] {
        built = ram::buildChaseCycle(m_latencyBuffer.data(), std::size_t(bytes), m_line, ram::chaseSeed(std::size_t(bytes)),
                                     cancelFn());
    });
    if (built == ram::CycleBuild::Cancelled) {
        m_stopped = true;
        return std::nullopt;
    }
    if (built == ram::CycleBuild::NoMemory) {
        const QString reason = QStringLiteral("%1: no memory for the cycle's index array").arg(stage);
        m_ctx.log(LogLevel::Error, reason);
        groupFail(kLatency, Status::Error, reason);
        return std::nullopt;
    }
    const ram::ChaseKernel kernel(m_latencyBuffer.data(), std::size_t(bytes), m_line, ram::hopsPerUnit(estimateNs));
    Measurement m = pool.measure(kernel, spec, cancelFn(), windowProgress(stage, spec, budgetAt));
    forcedFailure(m, stage);
    if (failed(kLatency, m, stage))
        return std::nullopt;
    const double ns = ram::latencyNs(m.median);
    if (!(ns > 0) || !std::isfinite(ns)) {
        const QString reason = QStringLiteral("%1: no hops were timed").arg(stage);
        m_ctx.log(LogLevel::Error, reason);
        groupFail(kLatency, Status::Error, reason);
        return std::nullopt;
    }
    if (spread)
        *spread = m.spread;
    return ns;
}

void RamRun::prepare()
{
    report(0, QStringLiteral("Preparing"));
    // §4.5: the background load in the second before the run.
    if (const auto load = cpu::sampleBackgroundLoadPercent(1000ms, cancelFn())) {
        m_r.metrics.append(number(QStringLiteral("background.load"), {}, QStringLiteral("Background load"), *load,
                                  QStringLiteral("%"), Direction::Neutral));
        if (*load >= kBackgroundWarnPercent)
            m_ctx.log(LogLevel::Warning, QStringLiteral("Background load %1% before the run: close other programs for repeatable "
                                                        "numbers")
                                             .arg(std::lround(*load)));
    }
    if (stopRequested())
        return;
    const QJsonObject battery = m_ctx.system().data.value(QStringLiteral("battery")).toObject();
    const bool onBattery = battery.value(QStringLiteral("present")).toBool() && !battery.value(QStringLiteral("ac_online")).toBool();
    m_r.metrics.append(text(QStringLiteral("power.source"), {}, QStringLiteral("Power source"),
                            onBattery ? QStringLiteral("Battery") : QStringLiteral("AC")));
    if (onBattery)
        m_ctx.log(LogLevel::Warning, QStringLiteral("On battery: plug in AC power for repeatable numbers"));
    if (const std::string profile = cpu::powerProfile(); !profile.empty())
        m_r.metrics.append(text(QStringLiteral("power.profile"), {}, QStringLiteral("Power profile"), qs(profile)));
    if (m_ctx.system().data.value(QStringLiteral("emulated")).toBool())
        m_r.metrics.append(text(QStringLiteral("emulated"), {}, QStringLiteral("Emulation"),
                                QStringLiteral("x64 build under emulation on ARM64")));
    m_r.metrics.append(number(QStringLiteral("threads"), {}, QStringLiteral("Threads used"), m_threads, QString(), Direction::Neutral));
    m_r.metrics.append(number(QStringLiteral("numa.nodes"), {}, QStringLiteral("NUMA nodes"), numaNodes(), QString(), Direction::Neutral));
    // §7: the readout, from the run's snapshot.
    for (const Metric &m : m_readout.metrics)
        m_r.metrics.append(m);
    if (!m_readout.available)
        m_ctx.log(LogLevel::Info, QStringLiteral("Memory details not available: %1").arg(m_readout.reason));

    // The core, its caches and the targets (§4.3, §5.2).
    m_core = pickCore();
    if (stopRequested())
        return;
    m_topology = isa::cacheTopologyFor(m_core.core);
    m_line = std::size_t(m_topology.chaseLineSize());
    m_targets = ram::targetsFromTopology(m_topology);
    m_dramBytes = m_targets.dramBytes;
    m_grid = ram::latencyGrid(m_dramBytes);
    planBudget();

    // The latency buffer (D18: fixed size; an Error if there isn't room).
    report(1.5, QStringLiteral("Preparing buffers"));
    if (const auto short_ = shortOfMemory(m_dramBytes)) {
        m_latencyProblem = QStringLiteral("The latency buffer %1").arg(*short_);
    } else {
        m_latencyBuffer = ram::HugeBuffer::allocate(std::size_t(m_dramBytes));
        if (!m_latencyBuffer.valid())
            m_latencyProblem = QStringLiteral("The latency buffer: %1").arg(qs(m_latencyBuffer.error()));
    }
    if (m_latencyProblem.isEmpty()) {
        bool touched = false;
        onCore(m_core.core, [&] { touched = m_latencyBuffer.firstTouch(cancelFn()); });
        if (!touched || stopRequested()) {
            m_stopped = true;
            return;
        }
        const ram::PageReport pages = m_latencyBuffer.settle(cancelFn());
        if (stopRequested())
            return;
        m_r.metrics.append(text(QStringLiteral("pages"), {}, QStringLiteral("Pages"), qs(pages.text)));
        m_r.metrics.append(number(QStringLiteral("pages.huge"), {}, QStringLiteral("Huge-page coverage"), pages.hugePercent,
                                  QStringLiteral("%"), Direction::Neutral));
        for (const std::string &n : pages.notes)
            m_ctx.log(LogLevel::Info, QStringLiteral("Pages: %1").arg(qs(n)));
        m_ctx.log(pages.mode == ram::PageReport::Mode::Huge ? LogLevel::Info : LogLevel::Warning,
                  QStringLiteral("Pages: %1 (%2% of the %3 latency buffer)")
                      .arg(qs(pages.text))
                      .arg(std::lround(pages.hugePercent))
                      .arg(sizeText(m_dramBytes)));
    }
    const QString where = m_core.core.ranked ? QStringLiteral("CPU %1").arg(coreName(m_core.core)) : QStringLiteral("unpinned");
    if (m_topology.empty()) {
        m_ctx.log(LogLevel::Info, QStringLiteral("Caches (%1): not reported by the OS; the levels come from the curve").arg(where));
        return;
    }
    m_ctx.log(LogLevel::Info, QStringLiteral("Caches (%1): %2").arg(where, cacheLevelsText(m_topology, m_targets)));
    QStringList t;
    for (int k = 1; k <= 3; ++k) {
        if (m_targets.level(k).present())
            t << QStringLiteral("L%1 %2").arg(k).arg(sizeText(m_targets.level(k).bytes));
    }
    t << QStringLiteral("DRAM %1").arg(sizeText(m_dramBytes));
    m_ctx.log(LogLevel::Info, QStringLiteral("Targets: %1").arg(t.join(QStringLiteral(" · "))));
}

void RamRun::latency()
{
    groupBegin(kLatency);
    m_r.metrics.append(text(QStringLiteral("latency.core"), kLatency, QStringLiteral("Latency core"), m_core.text));
    if (!m_latencyProblem.isEmpty()) {
        m_ctx.log(LogLevel::Error, m_latencyProblem);
        groupFail(kLatency, Status::Error, m_latencyProblem);
    } else {
        m_ctx.log(LogLevel::Info, QStringLiteral("Latency on %1").arg(m_core.text));
        if (latencyGrid() && !m_stopped)
            latencyTargets();
    }
    m_budgetDone = std::max(m_budgetDone, 2.5 + double(m_grid.size()) * gridPointBudget() + 4 * targetBudget());
    groupEnd(kLatency);
}

bool RamRun::latencyGrid()
{
    std::unique_ptr<WorkerPool> pool = chasePool(m_core.core);
    if (m_core.core.ranked && !pool->pinned())
        m_ctx.log(LogLevel::Warning, QStringLiteral("Latency: couldn't pin the thread to CPU %1; it runs where the OS puts it")
                                         .arg(coreName(m_core.core)));
    m_budgetDone = 2.5;
    double estimate = kFirstGridEstimateNs;
    int index = 0;
    for (const std::uint64_t bytes : m_grid) {
        const WindowSpec spec =
            cpu::scaled({ram::chaseWarmup(bytes, m_line, estimate, kGridWarmMin, kGridWarmMax), kGridRep, kReps});
        const auto ns = chase(*pool, m_core.core, bytes, estimate, spec, QStringLiteral("Latency curve · %1").arg(sizeText(bytes)),
                              m_budgetDone + gridPointBudget() * index++);
        if (!ns)
            return false;
        m_curve.push_back({bytes, *ns});
        estimate = *ns;
    }
    m_budgetDone += gridPointBudget() * double(m_grid.size());
    m_ctx.log(LogLevel::Info, QStringLiteral("Latency curve: %1 points, %2 to %3")
                                  .arg(m_grid.size())
                                  .arg(sizeText(m_grid.front()), sizeText(m_grid.back())));
    if (m_topology.empty()) {
        // §5.2's fallback: the levels from the curve's knees.
        m_targets = ram::targetsFromCurve(m_curve, m_dramBytes);
        m_ctx.log(LogLevel::Info, QStringLiteral("Cache levels: %1").arg(curveLevelsText(m_targets)));
    }
    return true;
}

void RamRun::latencyTargets()
{
    m_r.metrics.append(text(QStringLiteral("cache.levels"), kLatency, QStringLiteral("Cache levels"),
                            m_topology.empty() ? curveLevelsText(m_targets) : cacheLevelsText(m_topology, m_targets)));
    std::unique_ptr<WorkerPool> pool = chasePool(m_core.core);
    // A target's estimate: the nearest grid point, on a log axis (§5.1).
    const auto nearest = [&](std::uint64_t bytes) {
        double best = kFirstGridEstimateNs, dist = INFINITY;
        for (const ram::CurvePoint &p : m_curve) {
            const double d = std::abs(std::log2(double(p.bytes)) - std::log2(double(bytes)));
            if (d < dist) {
                dist = d;
                best = p.ns;
            }
        }
        return best;
    };
    struct Level {
        int key;  // 1-3, 4 = DRAM
        QString name;
        std::uint64_t bytes;
    };
    std::vector<Level> levels;
    for (int k = 1; k <= 3; ++k) {
        const ram::LevelTarget &l = m_targets.level(k);
        if (l.present())
            levels.push_back({k, QStringLiteral("L%1").arg(k), l.bytes});
        else if (k <= 2 || l.cacheBytes > 0)
            m_ctx.log(k <= 2 ? LogLevel::Warning : LogLevel::Info, QStringLiteral("L%1 not measured: %2").arg(k).arg(qs(l.absentReason)));
    }
    levels.push_back({4, QStringLiteral("DRAM"), m_dramBytes});
    int index = 0;
    for (const Level &l : levels) {
        const double estimate = nearest(l.bytes);
        const WindowSpec spec =
            cpu::scaled({ram::chaseWarmup(l.bytes, m_line, estimate, kTargetWarmMin, kTargetWarmMax), kTargetRep, kReps});
        double spread = 0;
        const auto ns = chase(*pool, m_core.core, l.bytes, estimate, spec, QStringLiteral("Latency %1").arg(l.name),
                              m_budgetDone + targetBudget() * index++, &spread);
        if (!ns)
            return;
        m_worstSpread = std::max(m_worstSpread, spread);
        m_targetNs[l.key] = *ns;
        const QString key = l.key == 4 ? QStringLiteral("latency.dram") : QStringLiteral("latency.l%1").arg(l.key);
        m_r.metrics.append(number(key, kLatency, QStringLiteral("%1 latency").arg(l.name), *ns, QStringLiteral("ns"),
                                  Direction::LowerIsBetter, l.key == 4));
        m_r.metrics.append(text(key + QStringLiteral(".at"), kLatency, QStringLiteral("%1 measured at").arg(l.name), sizeText(l.bytes)));
        // The target replaces the grid point of the same size (the DRAM
        // target always is one), else joins the curve (§2: sorted by x).
        const auto at = std::find_if(m_curve.begin(), m_curve.end(), [&](const ram::CurvePoint &p) { return p.bytes == l.bytes; });
        if (at != m_curve.end())
            at->ns = *ns;
        else
            m_curve.push_back({l.bytes, *ns});
        if (l.key == 4)
            m_dramNs = *ns;
    }
    std::sort(m_curve.begin(), m_curve.end(), [](const ram::CurvePoint &a, const ram::CurvePoint &b) { return a.bytes < b.bytes; });
    // Fewer than two cache levels (L1 and L2) found: the group can't be
    // scored (§5.2, the fallback's rule; a 1-level core the same).
    if (!m_targets.level(1).present() || !m_targets.level(2).present()) {
        const QString reason = QStringLiteral("the L1 and L2 couldn't both be measured (%1)")
                                   .arg(qs(!m_targets.level(1).present() ? m_targets.level(1).absentReason : m_targets.level(2).absentReason));
        m_ctx.log(LogLevel::Error, QStringLiteral("Latency: %1").arg(reason));
        groupFail(kLatency, Status::Error, reason);
    }

    // "Latency L1 0.8 · L2 2.6 · L3 9.6 · DRAM 74.2 ns (≈ 4 · 14 · 52 · 401
    // cycles at 5.4 GHz)": the cycles only when the chased core's clock can
    // be read (§4.4; never stored).
    QStringList parts, cycles;
    std::optional<double> mhz;
    if (m_core.core.ranked)
        mhz = cpu::telemetry::makeSystemSource()->clockMHz({false, m_core.core});
    for (const auto &[key, ns] : m_targetNs) {
        parts << QStringLiteral("%1 %2").arg(key == 4 ? QStringLiteral("DRAM") : QStringLiteral("L%1").arg(key), nsText(ns));
        if (mhz)
            cycles << QString::number(std::lround(ns * *mhz / 1000.0));
    }
    QString line = QStringLiteral("Latency %1 ns").arg(parts.join(QStringLiteral(" · ")));
    if (mhz && *mhz > 0)
        line += QStringLiteral(" (≈ %1 cycles at %2 GHz)").arg(cycles.join(QStringLiteral(" · "))).arg(*mhz / 1000.0, 0, 'f', 1);
    m_ctx.log(LogLevel::Info, line);
    // §5.2's sanity check: each level slower than the one inside it.
    double previous = 0;
    for (const auto &[key, ns] : m_targetNs) {
        if (ns <= previous) {
            m_ctx.log(LogLevel::Warning, QStringLiteral("The cache levels reported by the OS don't match the curve"));
            break;
        }
        previous = ns;
    }
}

bool RamRun::prepareBandwidth()
{
    m_plan = ram::planBandwidth(ram::lastLevelCaches(allTopologies(m_allowed)), m_threads, physicalMemory(m_ctx.system()));
    m_r.metrics.append(text(QStringLiteral("bandwidth.path"), kBandwidthId, QStringLiteral("Bandwidth path"),
                            qs(ram::bandwidthPathLabel(m_path))));
    m_r.metrics.append(text(QStringLiteral("bandwidth.buffer"), kBandwidthId, QStringLiteral("Bandwidth buffer"), qs(m_plan.describe())));
    if (m_plan.limited) {
        // P9 as revised (Dave, 2026-10-19): the cap was hit; the Detail page
        // shows this as a warn note.
        m_r.metrics.append(text(QStringLiteral("bandwidth.limited"), kBandwidthId, QStringLiteral("Bandwidth buffers limited"),
                                qs(m_plan.note)));
        m_ctx.log(LogLevel::Warning, qs(m_plan.note));
    }
    if (const auto short_ = shortOfMemory(m_plan.totalBytes())) {
        m_bandwidthProblem = QStringLiteral("The bandwidth buffers %1").arg(*short_);
        return false;
    }
    m_bwBuffer = ram::HugeBuffer::allocate(std::size_t(m_plan.totalBytes()));
    if (!m_bwBuffer.valid()) {
        m_bandwidthProblem = QStringLiteral("The bandwidth buffers: %1").arg(qs(m_bwBuffer.error()));
        return false;
    }
    for (int i = 0; i < m_plan.threads; ++i)
        m_slices.push_back(
            std::make_unique<ram::BandwidthSlice>(m_bwBuffer.data() + std::size_t(i) * m_plan.sliceBytes, m_plan.sliceBytes));
    // §5.3, §4.2: each slice first-touched by a thread pinned to its own
    // CPU, the one whose all-thread worker streams it (NUMA-local).
    report(m_budgetDone, QStringLiteral("Preparing bandwidth buffers"));
    std::atomic<bool> stopped{false};
    {
        std::vector<std::thread> fill;
        fill.reserve(m_slices.size());
        for (std::size_t i = 0; i < m_slices.size(); ++i) {
            fill.emplace_back([&, i] {
                if (i < m_allowed.size())
                    cpu::pinCurrentThread(m_allowed[i]);
                if (!m_slices[i]->fill(std::uint64_t(i) + 1, cancelFn()))
                    stopped = true;
            });
        }
        for (std::thread &t : fill)
            t.join();
    }
    if (stopped || stopRequested()) {
        m_stopped = true;
        return false;
    }
    const ram::PageReport pages = m_bwBuffer.settle(cancelFn());
    if (stopRequested())
        return false;
    m_ctx.log(LogLevel::Info, QStringLiteral("Bandwidth buffers: %1; %2").arg(qs(m_plan.describe()), qs(pages.text)));
    return true;
}

void RamRun::bandwidthStep(WorkerPool &pool, bool single)
{
    std::vector<ram::BandwidthSlice *> slices;
    for (const auto &s : m_slices)
        slices.push_back(s.get());
    std::map<std::string, double> &out = single ? m_st : m_mt;
    const WindowSpec spec = cpu::scaled(kBandwidth);
    for (const ram::BwVariant &v : m_variants) {
        if (m_stopped || m_groupStatus[kBandwidthId] != Status::Ok)
            break;
        const ram::BandwidthKernel kernel(m_path, v, slices, single ? 1 : pool.threads());
        const QString stage = variantStage(v, single);
        Measurement m = pool.measure(kernel, spec, cancelFn(), windowProgress(stage, spec, m_budgetDone));
        m_budgetDone += budget(spec);
        forcedFailure(m, stage);
        if (failed(kBandwidthId, m, variantLabel(v, single)))
            break;
        const double gbs = m.median / 1e9;
        out[v.key] = gbs;
        if (v.scored)
            m_worstSpread = std::max(m_worstSpread, m.spread);
        const QString key = (single ? QStringLiteral("bandwidth.st.") : QStringLiteral("bandwidth.")) + qs(v.key);
        m_r.metrics.append(number(key, kBandwidthId, variantLabel(v, single), gbs, QStringLiteral("GB/s"),
                                  v.scored ? Direction::HigherIsBetter : Direction::Neutral, !single && v.key == "read"));
        if (single)
            continue;
        // "Write 61.8 GB/s (non-temporal, spread 1.9%)" (the mockup's log).
        QStringList how;
        if (v.op != ram::BwOp::Read && kX64)
            how << (v.scored ? QStringLiteral("non-temporal") : QStringLiteral("plain stores, not scored"));
        if (v.op == ram::BwOp::Copy)
            how << QStringLiteral("read + written");
        how << QStringLiteral("spread %1%").arg(m.spread * 100, 0, 'f', 1);
        m_ctx.log(m.unstable() ? LogLevel::Warning : LogLevel::Info,
                  QStringLiteral("%1 %2 GB/s (%3)").arg(opName(v), gbsText(gbs), how.join(QStringLiteral(", "))));
    }
}

void RamRun::bandwidth()
{
    groupBegin(kBandwidthId);
    const double start = m_budgetDone;
    // §4.3: the core is picked again for the single-thread part.
    const CorePick pick = pickCore();
    if (stopRequested()) {
        groupEnd(kBandwidthId);
        return;
    }
    m_r.metrics.append(text(QStringLiteral("bandwidth.st.core"), kBandwidthId, QStringLiteral("Single-thread bandwidth core"), pick.text));
    if (!prepareBandwidth()) {
        if (!m_stopped) {
            m_ctx.log(LogLevel::Error, m_bandwidthProblem);
            groupFail(kBandwidthId, Status::Error, m_bandwidthProblem);
        }
        groupEnd(kBandwidthId);
        return;
    }
    m_budgetDone = std::max(m_budgetDone, start + seconds(m_sampleFor) + 1.5);

    const QString where = pick.core.ranked ? QStringLiteral("CPU %1").arg(coreName(pick.core)) : QStringLiteral("one unpinned thread");
    m_ctx.log(LogLevel::Info,
              QStringLiteral("Single thread on %1: %2")
                  .arg(where, m_path == ram::BwPath::Neon
                                  ? QStringLiteral("NEON loads and plain stores")
                                  : QStringLiteral("%1 loads; non-temporal stores scored, plain stores also shown")
                                        .arg(m_path == ram::BwPath::Avx2 ? QStringLiteral("AVX2") : QStringLiteral("SSE2"))));
    {
        const std::unique_ptr<WorkerPool> pool = chasePool(pick.core);
        bandwidthStep(*pool, true);
    }
    if (!m_stopped && m_groupStatus[kBandwidthId] == Status::Ok) {
        // "Single thread: read 41.0 · write 28.6 · copy 36.2 GB/s (plain:
        // write 22.4 · copy 27.9)".
        QString line = QStringLiteral("Single thread: read %1 · write %2 · copy %3 GB/s")
                           .arg(gbsText(m_st["read"]), gbsText(m_st["write"]), gbsText(m_st["copy"]));
        if (m_st.count("write.plain"))
            line += QStringLiteral(" (plain: write %1 · copy %2)").arg(gbsText(m_st["write.plain"]), gbsText(m_st["copy.plain"]));
        m_ctx.log(LogLevel::Info, line);
    }
    if (!m_stopped && m_groupStatus[kBandwidthId] == Status::Ok) {
        // §4.2, P11: one worker per logical CPU, worker i pinned to CPU i.
        WorkerPool::Options o;
        o.threads = m_plan.threads;
        o.pinEach = m_allowed;
        WorkerPool pool(o);
        if (pool.placementFailures() > 0)
            m_ctx.log(LogLevel::Warning, QStringLiteral("All threads: %1 workers couldn't be pinned to their CPU")
                                             .arg(pool.placementFailures()));
        m_ctx.log(LogLevel::Info, QStringLiteral("All threads: %1 %2, %3")
                                      .arg(m_plan.threads)
                                      .arg(m_allowed.empty() ? QStringLiteral("unpinned") : QStringLiteral("pinned"))
                                      .arg(qs(m_plan.describe())));
        bandwidthStep(pool, false);
    }
    // D12, §9: the read efficiency, and the warning when read beats the peak.
    if (m_mt.count("read") && m_readout.peakGBs && *m_readout.peakGBs > 0) {
        const double read = m_mt["read"];
        m_r.metrics.append(number(QStringLiteral("config.efficiency"), {}, QStringLiteral("Read efficiency"),
                                  100.0 * read / *m_readout.peakGBs, QStringLiteral("%"), Direction::Neutral));
        if (read > kPeakWarnFactor * *m_readout.peakGBs)
            m_ctx.log(LogLevel::Warning, QStringLiteral("All-thread read %1 GB/s is faster than the memory's theoretical peak "
                                                        "(%2 GB/s): SMBIOS may report the wrong speed")
                                             .arg(gbsText(read), gbsText(*m_readout.peakGBs)));
    }
    groupEnd(kBandwidthId);
}

void RamRun::loaded()
{
    groupBegin(kLoadedId);
    const auto give = [&](Status status, const QString &reason) {
        m_ctx.log(status == Status::Error ? LogLevel::Error : LogLevel::Warning, QStringLiteral("Loaded latency: %1").arg(reason));
        groupFail(kLoadedId, status, reason);
        groupEnd(kLoadedId);
    };
    // §5.4: the load needs a core of its own.
    const int cores = physicalCores(m_ctx.system());
    if ((cores > 0 ? cores : m_threads) < 2)
        return give(Status::NotAvailable, QStringLiteral("needs at least two cores"));
    if (!m_latencyBuffer.valid())
        return give(Status::Error, QStringLiteral("needs the latency buffer (%1)").arg(m_latencyProblem));
    if (m_slices.empty())
        return give(Status::Error, QStringLiteral("needs the bandwidth buffers, which weren't made"));

    const CorePick pick = pickCore();
    if (stopRequested()) {
        groupEnd(kLoadedId);
        return;
    }
    // P12: a load thread on every allowed CPU but the chased core's physical
    // core, each streaming the slice its own CPU first-touched.
    std::vector<ram::LoadThreadSpec> specs;
    for (const isa::CoreChoice &c : ram::loadCpus(m_allowed, pick.smtKeys)) {
        std::size_t i = 0;
        while (i + 1 < m_slices.size() && i < m_allowed.size() && isa::cpuKey(m_allowed[i]) != isa::cpuKey(c))
            ++i;
        specs.push_back({c, m_slices[i]->data(), m_slices[i]->bytes()});
    }
    if (specs.empty())
        return give(Status::NotAvailable, QStringLiteral("needs at least two cores"));
    m_r.metrics.append(text(QStringLiteral("loaded.core"), kLoadedId, QStringLiteral("Loaded-latency core"), pick.text));
    m_r.metrics.append(number(QStringLiteral("loaded.threads"), kLoadedId, QStringLiteral("Load threads"), double(specs.size()),
                              QString(), Direction::Neutral));

    // The DRAM cycle, rebuilt once on the chase core (§3).
    report(m_budgetDone, QStringLiteral("Loaded latency · preparing"));
    ram::CycleBuild built = ram::CycleBuild::Ok;
    onCore(pick.core, [&] {
        built = ram::buildChaseCycle(m_latencyBuffer.data(), std::size_t(m_dramBytes), m_line,
                                     ram::chaseSeed(std::size_t(m_dramBytes)), cancelFn());
    });
    if (built == ram::CycleBuild::Cancelled || stopRequested()) {
        m_stopped = true;
        groupEnd(kLoadedId);
        return;
    }
    if (built == ram::CycleBuild::NoMemory)
        return give(Status::Error, QStringLiteral("no memory for the cycle's index array"));

    std::unique_ptr<ram::LoadGenerator> load;
    try {
        load = std::make_unique<ram::LoadGenerator>(m_path, specs);
    } catch (const std::exception &e) {
        return give(Status::Error, QStringLiteral("couldn't start the load threads: %1").arg(QString::fromLocal8Bit(e.what())));
    }
    if (load->placementFailures() > 0)
        m_ctx.log(LogLevel::Warning, QStringLiteral("Loaded latency: %1 load threads couldn't be pinned to their CPU")
                                         .arg(load->placementFailures()));
    const QString core = pick.core.ranked ? QStringLiteral("CPU %1").arg(coreName(pick.core)) : QStringLiteral("one thread");
    const int siblings = int(pick.smtKeys.size()) - 1;
    const std::unique_ptr<WorkerPool> pool = chasePool(pick.core);
    // K from the idle DRAM latency; if that wasn't measured (the latency
    // part failed), from a typical one (§5.1's bounds keep Stop's 100 ms).
    const ram::ChaseKernel kernel(m_latencyBuffer.data(), std::size_t(m_dramBytes), m_line,
                                  ram::hopsPerUnit(m_dramNs.value_or(kDefaultDramEstimateNs)));
    const WindowSpec spec = cpu::scaled(kLoaded);
    const double base = m_budgetDone;
    ram::LoadedResult res = ram::measureLoadedLatency(*pool, kernel, *load, spec, cancelFn(), [&](int level, int window) {
        const double delay = ram::kLoadDelaysNs[level];
        const QString name = delay == 0 ? QStringLiteral("Loaded latency, full load")
                                        : QStringLiteral("Loaded latency, %1 ns delay").arg(Units::formatNumber(delay));
        windowProgress(name, spec, base + budget(spec) * level)(window);
    });
    load.reset();  // parked and joined before anything else happens
    m_budgetDone = base + budget(spec) * double(std::size(ram::kLoadDelaysNs));
    m_ctx.log(LogLevel::Info,
              QStringLiteral("Loaded latency on %1: %2 load threads%3; delay calibration %4 ns per spin")
                  .arg(core)
                  .arg(specs.size())
                  .arg(siblings > 0 ? QStringLiteral(" (not %1's SMT sibling%2)").arg(core, siblings > 1 ? QStringLiteral("s") : QString())
                                    : QString())
                  .arg(res.nsPerSpin, 0, 'f', 3));
    if (res.ok() && !res.levels.empty()) {
        Measurement m;
        m.median = 1;
        forcedFailure(m, QStringLiteral("Loaded latency"));
        if (!m.ok()) {
            res.outcome = m.outcome;
            res.error = m.error;
        }
    }
    if (!res.ok()) {
        Measurement m;
        m.outcome = res.outcome;
        m.error = res.error;
        failed(kLoadedId, m, QStringLiteral("Loaded latency"));
        groupEnd(kLoadedId);
        return;
    }
    Series curve{QStringLiteral("loaded_curve"), QStringLiteral("Loaded latency"), QStringLiteral("Load bandwidth"),
                 QStringLiteral("GB/s"), QStringLiteral("ns"), {}};
    if (m_dramNs)
        curve.points.append({0.0, *m_dramNs});  // the idle point (D7)
    for (const ram::LoadedLevel &l : res.levels) {
        curve.points.append({l.loadGBs, l.latencyNs});
        m_ctx.log(LogLevel::Info, QStringLiteral("%1: %2 ns at %3 GB/s (spread %4%)")
                                      .arg(l.delayNs == 0 ? QStringLiteral("Full load")
                                                          : QStringLiteral("%1 ns delay").arg(Units::formatNumber(l.delayNs)),
                                           nsText(l.latencyNs), gbsText(l.loadGBs))
                                      .arg(l.spread * 100, 0, 'f', 1));
    }
    std::stable_sort(curve.points.begin(), curve.points.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    const ram::LoadedLevel &full = res.levels.front();  // kLoadDelaysNs[0] = 0: full load, the scored point
    m_worstSpread = std::max(m_worstSpread, full.spread);
    m_r.metrics.append(number(QStringLiteral("loaded.latency"), kLoadedId, QStringLiteral("Loaded latency"), full.latencyNs,
                              QStringLiteral("ns"), Direction::LowerIsBetter, true));
    m_r.metrics.append(number(QStringLiteral("loaded.load"), kLoadedId, QStringLiteral("Load during loaded latency"), full.loadGBs,
                              QStringLiteral("GB/s"), Direction::Neutral));
    m_r.series.append(curve);
    groupEnd(kLoadedId);
}

void RamRun::finish(TestResult &r)
{
    if (!m_curve.empty()) {
        // §2: the grid and the targets, in KiB; a point that wasn't measured
        // is never in it (no zero on the log axis).
        Series curve{QStringLiteral("latency_curve"), QStringLiteral("Latency by buffer size"), QStringLiteral("Buffer size"),
                     QStringLiteral("KiB"), QStringLiteral("ns"), {}};
        for (const ram::CurvePoint &p : m_curve)
            curve.points.append({double(p.bytes) / double(KiB), p.ns});
        r.series.prepend(curve);
    }
    r.metrics.append(number(QStringLiteral("stability.spread"), {}, QStringLiteral("Worst spread"), m_worstSpread * 100,
                            QStringLiteral("%"), Direction::Neutral));
}

TestResult RamRun::run()
{
    m_grid = ram::latencyGrid(m_dramBytes);
    planBudget();
    const int estimate = kX64 ? RamTest::kBudgetSecondsX64 : RamTest::kBudgetSecondsArm64;
    m_ctx.log(LogLevel::Info, QStringLiteral("Starting RAM: %1 threads, about %2 min %3 s").arg(m_threads).arg(estimate / 60).arg(estimate % 60));
    // P5: no power throttling (EcoQoS) for the test on Windows; each worker
    // opts out too (the engine and the load generator).
    const cpu::ScopedProcessPowerThrottlingOptOut noEcoQos;

    prepare();
    if (!m_stopped)
        latency();
    if (!m_stopped)
        bandwidth();
    if (!m_stopped)
        loaded();

    TestResult r = std::move(m_r);
    finish(r);
    for (const QString &id : {kLatency, kBandwidthId, kLoadedId}) {
        const auto it = m_groupStatus.find(id);
        if (it == m_groupStatus.end())
            continue;  // never started (Stop)
        r.subResults.append({id, it->second, m_groupReason[id], {}});
    }
    if (m_stopped) {
        r.status = Status::Cancelled;
        return r;
    }
    int ok = 0;
    QString firstProblem;
    for (const SubResult &s : std::as_const(r.subResults)) {
        if (s.status == Status::Ok)
            ++ok;
        else if (firstProblem.isEmpty())
            firstProblem = s.reason;
    }
    if (ok == 3) {
        r.status = Status::Ok;
    } else if (ok > 0) {
        r.status = Status::Partial;
        r.statusReason = QStringLiteral("No RAM score: not every part ran");
    } else {
        r.status = Status::Error;
        r.statusReason = firstProblem;
    }
    m_ctx.progress(1.0, QStringLiteral("Done"));
    return r;
}

} // namespace

RamTest::RamTest()
{
    m_info.id = QStringLiteral("ram");
    m_info.name = QStringLiteral("RAM");
    m_info.summary = QStringLiteral("Latency curve, bandwidth, loaded latency");
    m_info.colorToken = QStringLiteral("t-ram");
    // 0 until calibration freezes the anchors (task 14), then 1 (§2).
    m_info.testVersion = 0;
    m_info.formulaVersion = 0;
    // D15: every run runs all three; none is selectable.
    m_info.subTests = {{kLatency, QStringLiteral("Latency"), false, true},
                       {kBandwidthId, QStringLiteral("Bandwidth"), false, true},
                       {kLoadedId, QStringLiteral("Loaded latency"), false, true}};
}

Availability RamTest::probe(const ProbeContext &ctx) const
{
    Availability a;
    const ram::PageSupport pages = ram::pageSupport();
    a.capabilities.append({pages.huge ? QStringLiteral("Huge pages") : QStringLiteral("4 KiB pages"),
                           pages.huge ? Tone::Good : Tone::Neutral});
    if (const int nodes = numaNodes(); nodes > 1)
        a.capabilities.append({QStringLiteral("NUMA: %1 nodes").arg(nodes), Tone::Neutral});
    const int cores = physicalCores(ctx.system());
    if ((cores > 0 ? cores : cpu::logicalCpuCount()) < 2) {
        a.state = Availability::State::Partial;
        a.reason = QStringLiteral("Loaded latency needs at least two cores");
        a.subTestUnavailable.insert(kLoadedId, a.reason);
    }
    // D18: a warning, never Unavailable, when free memory is under the peak
    // footprint + 512 MiB (ramPeakFootprint: P9 as revised).
    const std::vector<isa::CoreChoice> allowed = isa::allowedLogicalCpus();
    const int threads = allowed.empty() ? cpu::logicalCpuCount() : int(allowed.size());
    const isa::CacheTopology top = isa::cacheTopologyFor(isa::bestCoreForSingleThread());
    const isa::CacheLevel *last = top.lastLevel();
    const ram::BandwidthPlan plan =
        ram::planBandwidth(ram::lastLevelCaches(allTopologies(allowed)), threads, physicalMemory(ctx.system()));
    const std::uint64_t need = ram::ramPeakFootprint(ram::dramTarget(last ? last->sizeBytes : 0), plan) + kFreeMargin;
    if (const auto free = cpu::availableMemoryBytes(); free && *free < need)
        a.capabilities.append({QStringLiteral("Low free memory (%1 MiB, %2 MiB wanted)").arg(*free >> 20).arg(need >> 20), Tone::Warn});
    return a;
}

OptionSchema RamTest::options(const ProbeContext &ctx, const Availability &availability) const
{
    // §2: info rows only; the stub's buffer and duration options are retired.
    OptionSchema o;
    const auto info = [&](const char *key, const char *label, const QString &value, Tone tone = Tone::Neutral) {
        OptionDef d;
        d.key = QLatin1StringView(key);
        d.label = QLatin1StringView(label);
        d.kind = OptionKind::Info;
        d.infoValue = value;
        d.infoTone = tone;
        o.options.append(d);
    };
    info("memory", "Memory", ram::memoryReadout(ctx.system(), kWindows).summary);
    const ram::PageSupport pages = ram::pageSupport();
    info("pages", "Pages", qs(pages.text), pages.huge ? Tone::Good : Tone::Neutral);
    const std::vector<isa::CoreChoice> allowed = isa::allowedLogicalCpus();
    info("threads", "Threads",
         QStringLiteral("All %1 for the all-thread parts").arg(allowed.empty() ? cpu::logicalCpuCount() : int(allowed.size())));
    if (availability.subTestUnavailable.contains(kLoadedId))
        info("loaded", "Loaded latency", availability.subTestUnavailable.value(kLoadedId), Tone::Warn);
    info("runs", "Runs",
         kX64 ? QStringLiteral("The RAM test always runs every part (about 2 min 40 s)")
              : QStringLiteral("The RAM test always runs every part (about 2 min)"));
    return o;
}

std::chrono::seconds RamTest::estimate(const OptionValues &) const
{
    return std::chrono::seconds(kX64 ? kBudgetSecondsX64 : kBudgetSecondsArm64);
}

std::chrono::seconds RamTest::timeLimit(const OptionValues &o) const
{
    // 2 x the estimate + 60 s (§2), at the developer time scale for the
    // windows; the buffers and cycle builds don't shrink with it, so never
    // under the floor below real time (the CPU test's rule).
    const double scale = stubTimeScale();
    const double secs = 2.0 * double(estimate(o).count()) * scale + 60.0;
    const long long floor = scale < 1.0 ? stubTimeLimitFloor().count() : 0;
    return std::chrono::seconds(std::max<long long>(floor, std::llround(secs)));
}

TestResult RamTest::run(RunContext &ctx, const OptionValues &)
{
    RamRun run(ctx);
    return run.run();
}

ScoreBreakdown RamTest::score(const TestResult &result) const
{
    return ram::formula_v1::score(result);
}

} // namespace sb::tests
