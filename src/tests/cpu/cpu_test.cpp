// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/cpu_test.h"

#include "core/units.h"
#include "isa/cpu_features.h"
#include "tests/common/stub_clock.h"
#include "tests/cpu/compress/compress_kernels.h"
#include "tests/cpu/cpu_context.h"
#include "tests/cpu/cpu_windows.h"
#include "tests/cpu/crypto_client.h"
#include "tests/cpu/engine/platform_threads.h"
#include "tests/cpu/engine/time_scale.h"
#include "tests/cpu/engine/worker_pool.h"
#include "tests/cpu/formula_v1.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/fp_kernel.h"
#include "tests/cpu/kernels/int_kernel.h"
#include "tests/cpu/kernels/nbody_kernel.h"
#include "tests/cpu/kernels/primes_kernel.h"
#include "tests/cpu/kernels/simd_kernel.h"
#include "tests/cpu/kernels/sort_kernel.h"
#include "tests/cpu/telemetry/sampler.h"
#include "tests/cpu/telemetry/telemetry.h"
#include "tests/cpu/telemetry/throttle.h"

#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace sb::tests {

using namespace std::chrono_literals;
using cpu::Measurement;
using cpu::WindowSpec;
using cpu::WorkerPool;
namespace tel = cpu::telemetry;

namespace {

// §3: the windows (warm-up, repetition length, repetitions) at real time.
constexpr WindowSpec kStInt{1000ms, 3000ms, 5};
constexpr WindowSpec kStFp{1000ms, 3000ms, 5};
constexpr WindowSpec kStCompress{1000ms, 4000ms, 5};
constexpr WindowSpec kMtInt{2000ms, 4000ms, 5};
constexpr WindowSpec kMtFp{2000ms, 4000ms, 5};
constexpr WindowSpec kMtPrimes{2000ms, 2000ms, 5};
constexpr WindowSpec kMtSimd{2000ms, 4000ms, 5};
constexpr WindowSpec kMtCompress{1000ms, 1600ms, 5};
constexpr WindowSpec kMtDecompress{1000ms, 1000ms, 5};
constexpr WindowSpec kCryptoHw{1000ms, 1400ms, 5};
constexpr WindowSpec kCryptoSw{1000ms, 800ms, 5};
constexpr WindowSpec kMtSort{2000ms, 2000ms, 5};
constexpr WindowSpec kMtPhysics{2000ms, 2400ms, 5};
// Decision D: the single-thread core's idle sample before each window.
constexpr std::chrono::milliseconds kCoreSample{250};
// §4.4: a background load from this logs a warning.
constexpr double kBackgroundWarnPercent = 10;

// The groups, in run (weight) order, with their sub-test ids (stable forever).
struct Group {
    const char *id;
    const char *name;
};
constexpr Group kGroups[] = {{"int", "Integer"},       {"fp", "Floating point + primes"},
                             {"simd", "SIMD"},          {"compress", "Compression"},
                             {"crypto", "Encryption"},  {"sort", "Sorting"},
                             {"physics", "Physics"}};

double seconds(std::chrono::milliseconds ms)
{
    return double(ms.count()) / 1000.0;
}

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

QString number(double value, const QString &unit)
{
    return Units::format(value, unit);
}

QString coreName(const isa::CoreChoice &c)
{
    if (c.group > 0)
        return QStringLiteral("%1:%2").arg(c.group).arg(c.cpu);
    return QString::number(c.cpu);
}

// "AES-NI + VAES, SHA-NI" from the worker's feature list.
QString cryptoPathText(const QString &features)
{
    const QStringList f = features.split(QLatin1Char(','), Qt::SkipEmptyParts);
    const bool x86 = f.contains(QStringLiteral("pclmul")) || f.contains(QStringLiteral("vaes"))
                     || (f.contains(QStringLiteral("sha")) && !f.contains(QStringLiteral("sha2")));
    QStringList parts;
    if (x86) {
        if (f.contains(QStringLiteral("aes")))
            parts << (f.contains(QStringLiteral("vaes")) ? QStringLiteral("AES-NI + VAES") : QStringLiteral("AES-NI"));
        if (f.contains(QStringLiteral("sha")))
            parts << QStringLiteral("SHA-NI");
    } else {
        if (f.contains(QStringLiteral("aes")))
            parts << QStringLiteral("ARMv8 AES");
        if (f.contains(QStringLiteral("sha2")))
            parts << QStringLiteral("ARMv8 SHA-256");
    }
    if (parts.isEmpty())
        return QStringLiteral("No AES or SHA instructions (software only)");
    return parts.join(QStringLiteral(", "));
}

Metric metric(const QString &key, const QString &sub, const QString &label, double value, const QString &unit,
              Direction direction = Direction::HigherIsBetter, bool headline = false)
{
    return {key, sub, label, value, unit, direction, headline};
}

Metric textMetric(const QString &key, const QString &sub, const QString &label, const QString &value)
{
    return {key, sub, label, value, {}, Direction::Neutral, false};
}

#if defined(SYNBENCH_DEV_HOOKS)
// The dev telemetry-injection hook (§14.3, dev builds only):
// SYNBENCH_CPU_FAKE_THROTTLE=<stage> (e.g. "xz compress") makes the clock
// fall 4% per repetition in that measurement (repetition 5: -16%) at 95 °C,
// so the throttling flag, its log line and the Detail note can be seen
// without a hot machine. The score is untouched: it comes from the rates.
class InjectingSource : public tel::Source
{
public:
    InjectingSource(std::unique_ptr<tel::Source> inner, QString stage) : m_inner(std::move(inner)), m_stage(std::move(stage)) {}
    // The stage and window now running (the coordinator; read by the sampler).
    void setPhase(const QString &stage, int window)
    {
        std::lock_guard lock(m_mutex);
        m_active = stage == m_stage;
        m_window = window;
    }
    std::optional<double> clockMHz(const tel::ClockScope &scope) override
    {
        const double base = m_inner->clockMHz(scope).value_or(4600.0);
        std::lock_guard lock(m_mutex);
        if (!m_active || m_window < 1)
            return base;
        return base * (1.0 - 0.04 * (m_window - 1));
    }
    std::optional<double> temperatureC() override
    {
        const auto t = m_inner->temperatureC();
        std::lock_guard lock(m_mutex);
        return m_active ? std::optional<double>(95.0) : t;
    }
    std::string temperatureLabel() const override { return m_inner->temperatureLabel(); }
    std::optional<std::uint64_t> throttleCount() override { return m_inner->throttleCount(); }
    std::optional<double> performanceLimitPercent() override { return m_inner->performanceLimitPercent(); }

private:
    std::unique_ptr<tel::Source> m_inner;
    const QString m_stage;
    std::mutex m_mutex;
    bool m_active = false;
    int m_window = 0;
};
#endif

std::shared_ptr<tel::Source> makeSource()
{
#if defined(SYNBENCH_DEV_HOOKS)
    const QString stage = qEnvironmentVariable("SYNBENCH_CPU_FAKE_THROTTLE");
    if (!stage.isEmpty())
        return std::make_shared<InjectingSource>(tel::makeSystemSource(), stage);
#endif
    return tel::makeSystemSource();
}

// One CPU test run: the sequence of §3, with telemetry and progress.
class CpuRun
{
public:
    explicit CpuRun(RunContext &ctx)
        // The sampler's 1 Hz follows the dev time scale (10 ms at 1%), so a
        // shortened run still has samples in its repetitions.
        : m_ctx(ctx), m_source(makeSource()), m_sensor(m_source->temperatureLabel()),
          m_sampler(m_source, cpu::scaledWindow(1000ms)), m_threads(cpu::logicalCpuCount())
    {
    }

    TestResult run();

private:
    // How one measurement went, with what the throttle rule and the chart need.
    struct Step {
        int code = 0;    // telemetry.windows code
        QString stage;   // the progress stage ("xz compress"), for the log
        Measurement m;
        std::vector<double> starts;  // each window's start (s since the sampler started); [0] = warm-up
        double end = 0;
    };

    bool cancelled() const { return m_ctx.cancelled(); }
    double budget(const WindowSpec &spec, bool single) const
    {
        const WindowSpec s = cpu::scaled(spec);
        return seconds(s.warmup) + seconds(s.rep) * s.reps + (single ? seconds(m_sampleFor) * (s.reps + 1) : 0);
    }
    void planBudget();
    void report();
    void enterWindow(const QString &stage, int window, int reps, double budgetAt, double length);

    Step measure(WorkerPool &pool, const cpu::Kernel &kernel, const WindowSpec &spec, const QString &stage, int code,
                 bool single);
    Step crypto(cpu::CryptoAlg alg, bool software, const WindowSpec &spec, const QString &stage, int code,
                cpu::CryptoOutcome &out);
    tel::WindowTrace trace(const Step &s, const std::vector<double> &rates) const;
    void checkThrottle(const Step &s, const std::vector<double> &rates);
    void checkThrottle(const Step &s);

    void groupBegin(int index);
    void groupEnd(int index, Status status, const QString &reason = {});
    bool groupFailed(int index, const Step &s, const QString &what);

    void prepare();
    void singleThread();
    void integer();
    void floatingPoint();
    void simd();
    void compression();
    void encryption();
    void sorting();
    void physics();
    void finish(TestResult &r);

    RunContext &m_ctx;
    // Read only by the sampler's thread once it has started (a Windows source
    // isn't thread-safe): the sensor name and the throttle counters are read
    // before, or after it has stopped.
    std::shared_ptr<tel::Source> m_source;
    const std::string m_sensor;
    tel::Sampler m_sampler;
    const int m_threads;
    const std::chrono::milliseconds m_sampleFor = cpu::scaledWindow(kCoreSample);

    // Progress (the sampler's thread reports between window starts).
    mutable std::mutex m_progressMutex;
    double m_budgetTotal = 1, m_budgetDone = 0;
    double m_windowBudgetAt = 0, m_windowLength = 0, m_windowStartedAt = 0;
    QString m_stage = QStringLiteral("Preparing");

    // What the run found.
    TestResult m_r;
    bool m_stopped = false;
    std::map<int, Status> m_groupStatus;
    std::map<int, QString> m_groupReason;
    std::vector<tel::WindowTrace> m_traces;
    std::vector<QPointF> m_windowMarks;
    double m_worstSpread = 0;
    std::optional<std::uint64_t> m_throttleBefore;

    // Shared inputs (made once in prepare(), read-only after).
    std::unique_ptr<cpu::CorpusData> m_corpus;
    QString m_corpusProblem;
    std::unique_ptr<cpu::PrimesKernel> m_primes;
    std::unique_ptr<cpu::SimdInputs> m_simdInputs;
    cpu::SimdChoice m_simd;
    std::unique_ptr<cpu::SortArena> m_sortArena;
    std::unique_ptr<WorkerPool> m_pool;  // all threads
};

void CpuRun::planBudget()
{
    double total = 3;  // prepare
    for (const WindowSpec &s : {kStInt, kStFp, kStCompress})
        total += budget(s, true);
    for (const WindowSpec &s : {kMtInt, kMtFp, kMtPrimes, kMtSimd, kMtSort, kMtSort, kMtPhysics})
        total += budget(s, false);
    total += double(cpu::kCodecCount) * (budget(kMtCompress, false) + budget(kMtDecompress, false)) + 2;
    total += 2 * budget(kCryptoHw, false) + 2 * budget(kCryptoSw, false) + 1;
    m_budgetTotal = total;
}

void CpuRun::report()
{
    double fraction;
    QString stage;
    {
        std::lock_guard lock(m_progressMutex);
        const double now = m_sampler.now();
        const double into = std::clamp(now - m_windowStartedAt, 0.0, m_windowLength);
        fraction = std::clamp((m_windowBudgetAt + into) / m_budgetTotal, 0.0, 0.999);
        // The app adds the elapsed time ("· 2:05", as in the mockup).
        stage = m_stage;
    }
    m_ctx.progress(fraction, stage);
}

void CpuRun::enterWindow(const QString &stage, int window, int reps, double budgetAt, double length)
{
#if defined(SYNBENCH_DEV_HOOKS)
    if (auto *inject = dynamic_cast<InjectingSource *>(m_source.get()))
        inject->setPhase(stage, window);
#endif
    {
        std::lock_guard lock(m_progressMutex);
        m_stage = window == 0 ? stage + QStringLiteral(" · warm-up")
                              : stage + QStringLiteral(" · rep %1 of %2").arg(window).arg(reps);
        m_windowBudgetAt = budgetAt;
        m_windowLength = length;
        m_windowStartedAt = m_sampler.now();
    }
    report();
}

CpuRun::Step CpuRun::measure(WorkerPool &pool, const cpu::Kernel &kernel, const WindowSpec &specIn, const QString &stage,
                             int code, bool single)
{
    const WindowSpec spec = cpu::scaled(specIn);
    Step s;
    s.code = code;
    s.stage = stage;
    const double base = m_budgetDone;
    const double sample = single ? seconds(m_sampleFor) : 0;
    s.m = pool.measure(
        kernel, spec, [this] { return cancelled(); },
        [&](int w) {
            const double t = m_sampler.now();
            s.starts.push_back(t);
            if (w == 0)
                m_windowMarks.push_back({t, double(code)});
            if (single)
                m_sampler.setScope({false, pool.currentCore()});
            const double at = base + sample * (w + 1) + (w == 0 ? 0 : seconds(spec.warmup) + seconds(spec.rep) * (w - 1));
            enterWindow(stage, w, spec.reps, at, w == 0 ? seconds(spec.warmup) : seconds(spec.rep));
        });
    s.end = m_sampler.now();
    m_budgetDone = base + budget(specIn, single);
    if (s.m.outcome == Measurement::Outcome::Cancelled)
        m_stopped = true;
    if (s.m.ok())
        m_worstSpread = std::max(m_worstSpread, s.m.spread);
    return s;
}

CpuRun::Step CpuRun::crypto(cpu::CryptoAlg alg, bool software, const WindowSpec &specIn, const QString &stage, int code,
                            cpu::CryptoOutcome &out)
{
    const WindowSpec spec = cpu::scaled(specIn);
    Step s;
    s.code = code;
    s.stage = stage;
    const double base = m_budgetDone;
    cpu::CryptoRunSpec run;
    run.alg = alg;
    run.software = software;
    run.threads = m_threads;
    run.windows = spec;
    out = cpu::CryptoClient::run(run, [this] { return cancelled(); }, [&](int w) {
        const double t = m_sampler.now();
        s.starts.push_back(t);
        if (w == 0)
            m_windowMarks.push_back({t, double(code)});
        const double at = base + (w == 0 ? 0 : seconds(spec.warmup) + seconds(spec.rep) * (w - 1));
        enterWindow(stage, w, spec.reps, at, w == 0 ? seconds(spec.warmup) : seconds(spec.rep));
    });
    s.end = m_sampler.now();
    m_budgetDone = base + budget(specIn, false);
    if (out.status == cpu::CryptoOutcome::Status::Cancelled)
        m_stopped = true;
    if (out.ok())
        m_worstSpread = std::max(m_worstSpread, out.spread);
    return s;
}

tel::WindowTrace CpuRun::trace(const Step &s, const std::vector<double> &rates) const
{
    tel::WindowTrace t;
    t.name = windowName(s.code).toStdString();
    t.repRates = rates;
    const std::vector<tel::Sample> samples = m_sampler.samples();
    for (std::size_t r = 0; r < rates.size(); ++r) {
        // Repetition r + 1 runs from its window start to the next one (or the end).
        if (r + 1 < s.starts.size()) {
            const double t1 = r + 2 < s.starts.size() ? s.starts[r + 2] : s.end;
            t.repClockMHz.push_back(tel::medianClockMHz(samples, s.starts[r + 1], t1));
        } else {
            t.repClockMHz.push_back(std::nullopt);
        }
    }
    if (!s.starts.empty())
        t.peakTemperatureC = tel::peakTemperatureC(samples, s.starts.front(), s.end);
    return t;
}

void CpuRun::checkThrottle(const Step &s)
{
    std::vector<double> rates;
    for (const cpu::RepSample &r : s.m.reps)
        rates.push_back(r.rate);
    checkThrottle(s, rates);
}

void CpuRun::checkThrottle(const Step &s, const std::vector<double> &rates)
{
    const tel::WindowTrace t = trace(s, rates);
    m_traces.push_back(t);
    const tel::ThrottleVerdict v = tel::detectThrottle({t}, std::nullopt, std::nullopt);
    if (!v.detected)
        return;
    const QString at = v.temperatureC ? QStringLiteral(" at %1 °C").arg(std::lround(*v.temperatureC)) : QString();
    if (!v.byRate)
        m_ctx.log(LogLevel::Warning, QStringLiteral("%1: clock fell %2 → %3 GHz (−%4%)%5")
                                         .arg(s.stage)
                                         .arg(*v.fromMHz / 1000, 0, 'f', 1)
                                         .arg(*v.toMHz / 1000, 0, 'f', 1)
                                         .arg(std::lround(v.dropPercent))
                                         .arg(at));
    else
        m_ctx.log(LogLevel::Warning, QStringLiteral("%1: the rate fell %2% from the first repetition to the last%3")
                                         .arg(s.stage)
                                         .arg(std::lround(v.dropPercent))
                                         .arg(at));
}

void CpuRun::groupBegin(int index)
{
    m_ctx.subTestBegin(QLatin1StringView(kGroups[index].id));
    m_groupStatus[index] = Status::Ok;
}

void CpuRun::groupEnd(int index, Status status, const QString &reason)
{
    if (m_groupStatus[index] == Status::Ok && status != Status::Ok) {
        m_groupStatus[index] = status;
        m_groupReason[index] = reason;
    }
    if (m_stopped && m_groupStatus[index] == Status::Ok)
        m_groupStatus[index] = Status::Cancelled;
    m_ctx.subTestEnd(QLatin1StringView(kGroups[index].id), m_groupStatus[index]);
}

// A measurement that didn't give a result: the group is an Error (with the
// reason), or the run was stopped. True if so.
bool CpuRun::groupFailed(int index, const Step &s, const QString &what)
{
    if (s.m.ok())
        return false;
    if (s.m.outcome != Measurement::Outcome::Cancelled) {
        const QString reason = qs(s.m.error);
        m_ctx.log(LogLevel::Error, QStringLiteral("%1: %2").arg(what, reason));
        if (index >= 0 && m_groupStatus[index] == Status::Ok) {
            m_groupStatus[index] = Status::Error;
            m_groupReason[index] = reason;
        }
    }
    return true;
}

void CpuRun::prepare()
{
    enterWindow(QStringLiteral("Preparing"), 0, 0, 0, 3);
    // §4.4: the background load in the second before the run, before any of
    // our own work.
    if (const auto load = cpu::sampleBackgroundLoadPercent(1000ms, [this] { return cancelled(); })) {
        m_r.metrics.append(metric(QStringLiteral("background.load"), {}, QStringLiteral("Background load"), *load,
                                  QStringLiteral("%"), Direction::Neutral));
        if (*load >= kBackgroundWarnPercent)
            m_ctx.log(LogLevel::Warning,
                      QStringLiteral("Background load %1% before the run: close other programs for repeatable numbers")
                          .arg(std::lround(*load)));
    }
    if (cancelled()) {
        m_stopped = true;
        return;
    }
    const QJsonObject battery = m_ctx.system().data.value(QStringLiteral("battery")).toObject();
    const bool onBattery = battery.value(QStringLiteral("present")).toBool() && !battery.value(QStringLiteral("ac_online")).toBool();
    m_r.metrics.append(textMetric(QStringLiteral("power.source"), {}, QStringLiteral("Power source"),
                                  onBattery ? QStringLiteral("Battery") : QStringLiteral("AC")));
    if (onBattery)
        m_ctx.log(LogLevel::Warning, QStringLiteral("On battery: plug in AC power for repeatable numbers"));
    if (const std::string profile = cpu::powerProfile(); !profile.empty())
        m_r.metrics.append(textMetric(QStringLiteral("power.profile"), {}, QStringLiteral("Power profile"), qs(profile)));
    if (m_ctx.system().data.value(QStringLiteral("emulated")).toBool())
        m_r.metrics.append(textMetric(QStringLiteral("emulated"), {}, QStringLiteral("Emulation"),
                                      QStringLiteral("x64 build under emulation on ARM64")));
    if (!cpu::codecsOptimised())
        m_ctx.log(LogLevel::Warning, QStringLiteral("Debug build: the compression libraries aren't optimised, so these "
                                                    "numbers are for local checks only"));

    // The shared inputs.
    try {
        m_corpus = std::make_unique<cpu::CorpusData>(cpu::makeCorpusData(m_threads));
        if (m_corpus->crc != cpu::golden::kCorpusCrc32)
            m_corpusProblem = QStringLiteral("the generated corpus is wrong (CRC-32 %1, expected %2): possible "
                                             "miscompile or hardware fault")
                                  .arg(m_corpus->crc, 8, 16, QLatin1Char('0'))
                                  .arg(cpu::golden::kCorpusCrc32, 8, 16, QLatin1Char('0'));
    } catch (const std::exception &e) {
        m_corpus.reset();
        m_corpusProblem = QStringLiteral("couldn't make the corpus: %1").arg(QString::fromLocal8Bit(e.what()));
    }
    m_primes = std::make_unique<cpu::PrimesKernel>();
    m_simdInputs = std::make_unique<cpu::SimdInputs>();
    m_simd = cpu::chooseSimdPath(isa::detect(), *m_simdInputs);
    // "SIMD: AVX-512 path selected (…)": the path without its width, as in the mockup.
    QString simdNote = qs(m_simd.note);
    const QString pathName = QString::fromLatin1(isa::simdPathName(m_simd.path));
    simdNote.replace(pathName + QStringLiteral(" path"), pathName.section(QStringLiteral(" ("), 0, 0) + QStringLiteral(" path"));
    m_ctx.log(m_simd.warning ? LogLevel::Warning : LogLevel::Info, QStringLiteral("SIMD: %1").arg(simdNote));
    m_sortArena = std::make_unique<cpu::SortArena>();
    m_budgetDone = 3;
}

void CpuRun::singleThread()
{
    WorkerPool::Options o = WorkerPool::singleThreadOptions();
    o.sampleFor = m_sampleFor;
    if (o.pickFrom.empty()) {
        m_ctx.log(LogLevel::Info, QStringLiteral("Single-thread runs unpinned (no core ranking available)"));
    } else if (o.pickFrom.size() == 1) {
        m_ctx.log(LogLevel::Info, QStringLiteral("Single-thread pinned to CPU %1 (top-ranked core)").arg(coreName(o.pinTo)));
    } else {
        QStringList cores;
        for (const isa::CoreCandidate &c : o.pickFrom)
            cores << coreName(c.core);
        m_ctx.log(LogLevel::Info, QStringLiteral("Single-thread: top-ranked cores %1; each repetition runs on the quietest")
                                      .arg(cores.join(QStringLiteral(", "))));
    }
    WorkerPool pool(o);
    if (o.pinTo.ranked && !pool.pinned())
        m_ctx.log(LogLevel::Warning, QStringLiteral("Single-thread: couldn't pin the thread; it runs where the OS puts it"));

    // The cores each repetition ran on, for single.cpu / single.cores.
    std::vector<isa::CoreChoice> used;
    QStringList perMeasurement;
    bool anyUnpinned = false;
    const auto noteCores = [&](const char *what, const char *key, const Measurement &m) {
        QStringList list;
        for (std::size_t i = 0; i < m.reps.size(); ++i) {
            const cpu::RepSample &rep = m.reps[i];
            if (!rep.core.ranked) {
                anyUnpinned = true;
                list << QStringLiteral("-");
            } else {
                list << coreName(rep.core);
                if (std::none_of(used.begin(), used.end(), [&](const isa::CoreChoice &c) {
                        return c.cpu == rep.core.cpu && c.group == rep.core.group;
                    }))
                    used.push_back(rep.core);
            }
            if (rep.repickFailed)
                m_ctx.log(LogLevel::Warning, QStringLiteral("Single-thread %1: couldn't move to the quietest core before "
                                                            "repetition %2; it stayed on CPU %3")
                                                 .arg(QLatin1StringView(what))
                                                 .arg(i + 1)
                                                 .arg(coreName(rep.core)));
        }
        perMeasurement << QStringLiteral("%1 %2").arg(QLatin1StringView(what), list.join(QLatin1Char(',')));
        m_ctx.log(LogLevel::Info, QStringLiteral("%1 on CPU %2").arg(QLatin1StringView(key), list.join(QStringLiteral(", "))));
    };

    const cpu::IntKernel ik;
    Step s = measure(pool, ik, kStInt, QStringLiteral("Single-thread integer"), kWinSingleInt, true);
    if (m_stopped)
        return;
    if (!groupFailed(-1, s, QStringLiteral("Single-thread integer"))) {
        m_r.metrics.append(metric(QStringLiteral("single.int"), {}, QStringLiteral("Single-thread integer"),
                                  s.m.median / 1e6, QStringLiteral("MOps/s")));
        noteCores("integer", "single.int", s.m);
    }
    const cpu::FpKernel fk;
    s = measure(pool, fk, kStFp, QStringLiteral("Single-thread floating point"), kWinSingleFp, true);
    if (m_stopped)
        return;
    if (!groupFailed(-1, s, QStringLiteral("Single-thread floating point"))) {
        m_r.metrics.append(metric(QStringLiteral("single.fp"), {}, QStringLiteral("Single-thread floating point"),
                                  s.m.median / 1e6, QStringLiteral("MFLOPS")));
        noteCores("FP", "single.fp", s.m);
    }
    if (m_corpus && m_corpusProblem.isEmpty()) {
        const cpu::SingleCompressKernel ck(*m_corpus);
        s = measure(pool, ck, kStCompress, QStringLiteral("Single-thread compression"), kWinSingleCompress, true);
        if (m_stopped)
            return;
        if (!groupFailed(-1, s, QStringLiteral("Single-thread compression"))) {
            m_r.metrics.append(metric(QStringLiteral("single.compress"), {}, QStringLiteral("Single-thread compression"),
                                      s.m.median / 1e6, QStringLiteral("MB/s")));
            noteCores("compression", "single.compress", s.m);
        }
        if (ck.sizeMismatches() > 0)
            m_ctx.log(LogLevel::Warning, QStringLiteral("Single-thread compression: %1 compressed sizes differed from the "
                                                        "reference (a different library build?)")
                                             .arg(ck.sizeMismatches()));
    } else {
        m_ctx.log(LogLevel::Error, QStringLiteral("Single-thread compression: %1").arg(m_corpusProblem));
        m_budgetDone += budget(kStCompress, true);
    }
    m_sampler.setScope({true, {}});

    // The single-thread mark (§7.4), a headline metric.
    const Metric *si = m_r.metric(QStringLiteral("single.int"));
    const Metric *sf = m_r.metric(QStringLiteral("single.fp"));
    const Metric *sc = m_r.metric(QStringLiteral("single.compress"));
    if (si && sf && sc) {
        if (const auto mark = cpu::formula_v1::singleThreadMark(std::get<double>(si->value), std::get<double>(sf->value),
                                                                std::get<double>(sc->value))) {
            m_r.metrics.append(metric(QStringLiteral("single.mark"), {}, QStringLiteral("Single-thread mark"), *mark,
                                      QStringLiteral("points"), Direction::HigherIsBetter, true));
            m_ctx.log(LogLevel::Info, QStringLiteral("Single-thread mark %1 (integer %2 · FP %3)")
                                          .arg(number(*mark, QStringLiteral("points")),
                                               number(std::get<double>(si->value), QStringLiteral("MOps/s")),
                                               number(std::get<double>(sf->value), QStringLiteral("MFLOPS"))));
        }
    }

    // single.cpu (§2): which core(s) the single-thread mark ran on.
    QString where;
    if (!o.pinTo.ranked || anyUnpinned || used.empty()) {
        where = QStringLiteral("unpinned (no core ranking available)");
        if (o.pinTo.ranked)
            where = QStringLiteral("unpinned (pinning failed)");
    } else {
        QStringList names;
        for (const isa::CoreChoice &c : used)
            names << coreName(c);
        if (o.pickFrom.size() <= 1)
            where = QStringLiteral("CPU %1 (top-ranked core)").arg(names.join(QStringLiteral(", ")));
        else if (used.size() == 1)
            where = QStringLiteral("CPU %1 (the quietest top-ranked core)").arg(names.front());
        else
            where = QStringLiteral("CPU %1 (the quietest top-ranked core, chosen per repetition)")
                        .arg(names.join(QStringLiteral(" and ")));
    }
    m_r.metrics.append(textMetric(QStringLiteral("single.cpu"), {}, QStringLiteral("Single-thread core"), where));
    if (!perMeasurement.isEmpty())
        m_r.metrics.append(textMetric(QStringLiteral("single.cores"), {}, QStringLiteral("Single-thread core per repetition"),
                                      perMeasurement.join(QStringLiteral(" · "))));
}

void CpuRun::integer()
{
    groupBegin(0);
    const cpu::IntKernel k;
    const Step s = measure(*m_pool, k, kMtInt, QStringLiteral("Integer"), kWinInt, false);
    if (!groupFailed(0, s, QStringLiteral("Integer"))) {
        m_r.metrics.append(metric(QStringLiteral("int.rate"), QStringLiteral("int"), QStringLiteral("Integer"),
                                  s.m.median / 1e6, QStringLiteral("MOps/s")));
        m_ctx.log(LogLevel::Info, QStringLiteral("Integer %1 (spread %2%)")
                                      .arg(number(s.m.median / 1e6, QStringLiteral("MOps/s")))
                                      .arg(s.m.spread * 100, 0, 'f', 1));
        checkThrottle(s);
    }
    groupEnd(0, Status::Ok);
}

void CpuRun::floatingPoint()
{
    groupBegin(1);
    const cpu::FpKernel k;
    Step s = measure(*m_pool, k, kMtFp, QStringLiteral("Floating point"), kWinFp, false);
    std::optional<double> fp, primes;
    if (!groupFailed(1, s, QStringLiteral("Floating point"))) {
        fp = s.m.median / 1e6;
        m_r.metrics.append(metric(QStringLiteral("fp.rate"), QStringLiteral("fp"), QStringLiteral("Floating point"), *fp,
                                  QStringLiteral("MFLOPS")));
        checkThrottle(s);
    }
    if (!m_stopped) {
        s = measure(*m_pool, *m_primes, kMtPrimes, QStringLiteral("Primes"), kWinPrimes, false);
        if (!groupFailed(1, s, QStringLiteral("Primes"))) {
            primes = s.m.median / 1e6;
            m_r.metrics.append(metric(QStringLiteral("fp.primes"), QStringLiteral("fp"), QStringLiteral("Primes"), *primes,
                                      QStringLiteral("MOps/s")));
            checkThrottle(s);
        }
    }
    if (fp && primes)
        m_ctx.log(LogLevel::Info, QStringLiteral("Floating point %1 · primes %2")
                                      .arg(number(*fp, QStringLiteral("MFLOPS")), number(*primes, QStringLiteral("MOps/s"))));
    groupEnd(1, Status::Ok);
}

void CpuRun::simd()
{
    groupBegin(2);
    m_r.metrics.append(textMetric(QStringLiteral("simd.path"), QStringLiteral("simd"), QStringLiteral("SIMD path"), qs(m_simd.label)));
    if (m_simd.path == isa::SimdPath::None) {
        m_ctx.log(LogLevel::Error, QStringLiteral("SIMD: %1").arg(qs(m_simd.note)));
        m_budgetDone += budget(kMtSimd, false);
        groupEnd(2, Status::NotAvailable, qs(m_simd.note));
        return;
    }
    const cpu::SimdKernel k(*m_simdInputs, m_simd.path, m_simd.vectorBits);
    const Step s = measure(*m_pool, k, kMtSimd, QStringLiteral("SIMD"), kWinSimd, false);
    if (!groupFailed(2, s, QStringLiteral("SIMD"))) {
        m_r.metrics.append(metric(QStringLiteral("simd.rate"), QStringLiteral("simd"), QStringLiteral("SIMD"),
                                  s.m.median / 1e6, QStringLiteral("MOps/s")));
        m_ctx.log(LogLevel::Info, QStringLiteral("SIMD %1 (%2)")
                                      .arg(number(s.m.median / 1e6, QStringLiteral("MOps/s")),
                                           QString::fromLatin1(isa::simdPathName(m_simd.path)).section(QStringLiteral(" ("), 0, 0)));
        checkThrottle(s);
    }
    groupEnd(2, Status::Ok);
}

void CpuRun::compression()
{
    groupBegin(3);
    if (!m_corpus || !m_corpusProblem.isEmpty()) {
        m_ctx.log(LogLevel::Error, QStringLiteral("Compression: %1").arg(m_corpusProblem));
        m_budgetDone += double(cpu::kCodecCount) * (budget(kMtCompress, false) + budget(kMtDecompress, false)) + 2;
        groupEnd(3, Status::Error, m_corpusProblem);
        return;
    }
    for (std::size_t i = 0; i < cpu::kCodecCount && !m_stopped && m_groupStatus[3] == Status::Ok; ++i) {
        const cpu::Codec codec = cpu::kCodecs[i];
        const QString name = QString::fromLatin1(cpu::codecName(codec));
        const QString key = QString::fromLatin1(cpu::codecKey(codec));
        // The unscored prep step: the whole corpus compressed, for the
        // decompress units and the ratio.
        {
            std::lock_guard lock(m_progressMutex);
            m_stage = QStringLiteral("%1 prepare").arg(name);
        }
        report();
        std::optional<cpu::CompressedSet> set;
        try {
            set = cpu::compressCorpus(codec, *m_corpus, m_threads, [this] { return cancelled(); });
        } catch (const std::exception &e) {
            const QString reason = QStringLiteral("%1 failed: %2").arg(name, QString::fromLocal8Bit(e.what()));
            m_ctx.log(LogLevel::Error, QStringLiteral("Compression: %1").arg(reason));
            groupEnd(3, Status::Error, reason);
            return;
        }
        if (!set) {
            m_stopped = true;
            break;
        }
        m_budgetDone += 0.5;
        const cpu::CompressKernel ck(codec, *m_corpus);
        Step s = measure(*m_pool, ck, kMtCompress, QStringLiteral("%1 compress").arg(name), kWinCompress + int(i) * 2, false);
        std::optional<double> c, d;
        if (!groupFailed(3, s, QStringLiteral("%1 compress").arg(name))) {
            c = s.m.median / 1e6;
            m_r.metrics.append(metric(QStringLiteral("compress.%1.c").arg(key), QStringLiteral("compress"),
                                      QStringLiteral("%1 compress").arg(name), *c, QStringLiteral("MB/s")));
            checkThrottle(s);
        }
        if (ck.sizeMismatches() > 0 || set->sizeMismatches > 0)
            m_ctx.log(LogLevel::Warning, QStringLiteral("%1: %2 compressed sizes differed from the reference (a different "
                                                        "library build?)")
                                             .arg(name)
                                             .arg(ck.sizeMismatches() + set->sizeMismatches));
        if (m_stopped || m_groupStatus[3] != Status::Ok)
            break;
        const cpu::DecompressKernel dk(*m_corpus, *set);
        s = measure(*m_pool, dk, kMtDecompress, QStringLiteral("%1 decompress").arg(name), kWinCompress + int(i) * 2 + 1, false);
        if (!groupFailed(3, s, QStringLiteral("%1 decompress").arg(name))) {
            d = s.m.median / 1e6;
            m_r.metrics.append(metric(QStringLiteral("compress.%1.d").arg(key), QStringLiteral("compress"),
                                      QStringLiteral("%1 decompress").arg(name), *d, QStringLiteral("MB/s")));
            m_r.metrics.append(metric(QStringLiteral("compress.%1.ratio").arg(key), QStringLiteral("compress"),
                                      QStringLiteral("%1 ratio").arg(name), set->ratio(), QString(), Direction::Neutral));
            checkThrottle(s);
        }
        if (c && d)
            m_ctx.log(LogLevel::Info, QStringLiteral("%1 %2 / %3 MB/s")
                                          .arg(name, Units::formatNumber(*c, QStringLiteral("MB/s")),
                                               Units::formatNumber(*d, QStringLiteral("MB/s"))));
    }
    groupEnd(3, Status::Ok);
}

void CpuRun::encryption()
{
    groupBegin(4);
    if (cpu::CryptoClient::workerPath().isEmpty()) {
        const QString reason = QStringLiteral("The encryption worker (synbench-cpucrypto) is missing");
        m_ctx.log(LogLevel::Error, QStringLiteral("Encryption: %1").arg(reason));
        m_budgetDone += 2 * budget(kCryptoHw, false) + 2 * budget(kCryptoSw, false) + 1;
        groupEnd(4, Status::NotAvailable, reason);
        return;
    }
    struct Part {
        cpu::CryptoAlg alg;
        bool software;
        const char *key;
        const char *label;
        int code;
    };
    const Part parts[] = {
        {cpu::CryptoAlg::AesGcm, false, "crypto.aes_gcm", "AES-256-GCM", kWinAesHw},
        {cpu::CryptoAlg::Sha256, false, "crypto.sha256", "SHA-256", kWinShaHw},
        {cpu::CryptoAlg::AesGcm, true, "crypto.aes_gcm.sw", "AES-256-GCM", kWinAesSw},
        {cpu::CryptoAlg::Sha256, true, "crypto.sha256.sw", "SHA-256", kWinShaSw},
    };
    bool pathLogged = false;
    for (const Part &p : parts) {
        if (m_stopped)
            break;
        const QString label = QLatin1StringView(p.label);
        const QString stage = p.software ? label + QStringLiteral(" (software path)") : label;
        cpu::CryptoOutcome out;
        const Step s = crypto(p.alg, p.software, p.software ? kCryptoSw : kCryptoHw, stage, p.code, out);
        if (m_stopped)
            break;
        if (!out.ok()) {
            m_ctx.log(p.software ? LogLevel::Warning : LogLevel::Error, QStringLiteral("%1: %2").arg(stage, out.error));
            if (!p.software && m_groupStatus[4] == Status::Ok) {
                m_groupStatus[4] = Status::Error;
                m_groupReason[4] = out.error;
            }
            continue;
        }
        if (!p.software && !pathLogged) {
            pathLogged = true;
            const QString path = cryptoPathText(out.features);
            m_r.metrics.append(textMetric(QStringLiteral("crypto.path"), QStringLiteral("crypto"), QStringLiteral("Encryption path"), path));
            m_r.metrics.append(textMetric(QStringLiteral("crypto.sw_path"), QStringLiteral("crypto"), QStringLiteral("Software path"),
                                          QStringLiteral("AES and SHA instructions off (SIMD on)")));
            m_ctx.log(LogLevel::Info, QStringLiteral("Encryption: hardware %1; software path = AES/SHA off").arg(path));
            m_ctx.log(LogLevel::Info, QStringLiteral("Encryption worker: OpenSSL %1").arg(out.openssl));
        }
        m_r.metrics.append(metric(QLatin1StringView(p.key), QStringLiteral("crypto"),
                                  p.software ? label + QStringLiteral(" (software path)") : label, out.medianMbps,
                                  QStringLiteral("MB/s")));
        m_ctx.log(LogLevel::Info, QStringLiteral("%1 %2 (spread %3%)")
                                      .arg(stage, number(out.medianMbps, QStringLiteral("MB/s")))
                                      .arg(out.spread * 100, 0, 'f', 1));
        checkThrottle(s, out.repMbps);
    }
    groupEnd(4, Status::Ok);
}

void CpuRun::sorting()
{
    groupBegin(5);
    const cpu::SortKeysKernel keys;
    Step s = measure(*m_pool, keys, kMtSort, QStringLiteral("64-bit sort"), kWinSortU64, false);
    std::optional<double> u64, str;
    if (!groupFailed(5, s, QStringLiteral("64-bit sort"))) {
        u64 = s.m.median / 1e6;
        m_r.metrics.append(metric(QStringLiteral("sort.u64"), QStringLiteral("sort"), QStringLiteral("64-bit sort"), *u64,
                                  QStringLiteral("MOps/s")));
        checkThrottle(s);
    }
    if (!m_stopped) {
        const cpu::SortStringsKernel strings(*m_sortArena);
        s = measure(*m_pool, strings, kMtSort, QStringLiteral("String sort"), kWinSortStr, false);
        if (!groupFailed(5, s, QStringLiteral("String sort"))) {
            str = s.m.median / 1e6;
            m_r.metrics.append(metric(QStringLiteral("sort.str"), QStringLiteral("sort"), QStringLiteral("String sort"), *str,
                                      QStringLiteral("MOps/s")));
            checkThrottle(s);
        }
    }
    if (u64 && str)
        m_ctx.log(LogLevel::Info, QStringLiteral("Sorting: 64-bit %1 · strings %2")
                                      .arg(number(*u64, QStringLiteral("MOps/s")), number(*str, QStringLiteral("MOps/s"))));
    groupEnd(5, Status::Ok);
}

void CpuRun::physics()
{
    groupBegin(6);
    const cpu::NbodyKernel k(m_pool->threads());
    const Step s = measure(*m_pool, k, kMtPhysics, QStringLiteral("Physics"), kWinPhysics, false);
    if (!groupFailed(6, s, QStringLiteral("Physics"))) {
        m_r.metrics.append(metric(QStringLiteral("physics.nbody"), QStringLiteral("physics"), QStringLiteral("Physics (n-body)"),
                                  s.m.median / 1e6, QStringLiteral("MOps/s")));
        m_ctx.log(LogLevel::Info, QStringLiteral("Physics %1").arg(number(s.m.median / 1e6, QStringLiteral("MOps/s"))));
        checkThrottle(s);
    }
    groupEnd(6, Status::Ok);
}

void CpuRun::finish(TestResult &r)
{
    // After a Stop the sampler gets 50 ms to end: a slow sensor read must not
    // hold up the 250 ms Stop contract (it ends by itself; the next run joins it).
    std::optional<std::uint64_t> throttleAfter;
    if (m_stopped) {
        if (m_sampler.stopWithin(50ms))
            throttleAfter = m_source->throttleCount();
    } else {
        m_sampler.stop();
        throttleAfter = m_source->throttleCount();
    }
    const std::vector<tel::Sample> samples = m_sampler.samples();

    // Telemetry: the series and their summaries.
    Series clock{QStringLiteral("telemetry.clock"), QStringLiteral("Clock (CPUs running the test)"), QStringLiteral("Time"),
                 QStringLiteral("s"), QStringLiteral("MHz"), {}};
    const std::string &sensor = m_sensor;
    Series temp{QStringLiteral("telemetry.temp"),
                sensor.empty() ? QStringLiteral("Temperature") : QStringLiteral("Temperature (%1)").arg(qs(sensor)),
                QStringLiteral("Time"), QStringLiteral("s"), QStringLiteral("°C"), {}};
    double clockSum = 0;
    int clockCount = 0;
    std::optional<double> peak;
    for (const tel::Sample &s : samples) {
        if (s.clockMHz) {
            clock.points.append({s.t, *s.clockMHz});
            clockSum += *s.clockMHz;
            ++clockCount;
        }
        if (s.temperatureC) {
            temp.points.append({s.t, *s.temperatureC});
            if (!peak || *s.temperatureC > *peak)
                peak = s.temperatureC;
        }
    }
    if (!clock.points.isEmpty())
        r.series.append(clock);
    if (!temp.points.isEmpty())
        r.series.append(temp);
    if (!m_windowMarks.empty()) {
        Series marks{QStringLiteral("telemetry.windows"), QStringLiteral("Measurement windows"), QStringLiteral("Time"),
                     QStringLiteral("s"), QString(), {}};
        for (const QPointF &p : m_windowMarks)
            marks.points.append(p);
        r.series.append(marks);
    }
    if (clockCount > 0)
        r.metrics.append(metric(QStringLiteral("clock.avg"), {}, QStringLiteral("Average clock"), clockSum / clockCount,
                                QStringLiteral("MHz"), Direction::Neutral));
    if (peak)
        r.metrics.append(metric(QStringLiteral("temp.max"), {},
                                sensor.empty() ? QStringLiteral("Peak temperature")
                                               : QStringLiteral("Peak temperature (%1)").arg(qs(sensor)),
                                *peak, QStringLiteral("°C"), Direction::Neutral));

    // The throttling flag (§8.2).
    const tel::ThrottleVerdict v = tel::detectThrottle(m_traces, m_throttleBefore, throttleAfter);
    r.metrics.append(textMetric(QStringLiteral("throttle"), {}, QStringLiteral("Throttling"),
                                v.detected ? QStringLiteral("Detected") : QStringLiteral("Not detected")));
    if (v.detected) {
        r.metrics.append(textMetric(QStringLiteral("throttle.detail"), {}, QStringLiteral("Throttling detail"), qs(tel::throttleDetail(v))));
        // The Detail page's note, as approved (it needs the temperature,
        // which throttle.detail doesn't carry).
        r.metrics.append(textMetric(QStringLiteral("throttle.note"), {}, QStringLiteral("Throttling note"), qs(tel::throttleNote(v))));
        m_ctx.log(LogLevel::Warning, qs(tel::throttleNote(v)));
    }
    r.metrics.append(metric(QStringLiteral("stability.spread"), {}, QStringLiteral("Worst spread"), m_worstSpread * 100,
                            QStringLiteral("%"), Direction::Neutral));
    r.metrics.append(metric(QStringLiteral("threads"), {}, QStringLiteral("Threads used"), m_threads, QString(), Direction::Neutral));
}

TestResult CpuRun::run()
{
    planBudget();
    tel::Sampler::joinAbandoned();
    m_throttleBefore = m_source->throttleCount();
    m_sampler.setOnSample([this](const tel::Sample &) { report(); });
    m_sampler.start();
    m_ctx.log(LogLevel::Info, QStringLiteral("Starting CPU: %1 threads, about %2")
                                  .arg(m_threads)
                                  .arg(QStringLiteral("4 min 30 s")));

    prepare();
    if (!m_stopped)
        singleThread();
    if (!m_stopped) {
        m_pool = std::make_unique<WorkerPool>(WorkerPool::Options{m_threads, {}});
        if (m_pool->placementFailures() > 0)
            m_ctx.log(LogLevel::Warning, QStringLiteral("%1 worker threads couldn't be placed on their processor group")
                                             .arg(m_pool->placementFailures()));
    }
    using Body = void (CpuRun::*)();
    const Body groups[] = {&CpuRun::integer,     &CpuRun::floatingPoint, &CpuRun::simd,   &CpuRun::compression,
                           &CpuRun::encryption,  &CpuRun::sorting,       &CpuRun::physics};
    for (const Body g : groups) {
        if (m_stopped)
            break;
        (this->*g)();
    }
    m_pool.reset();

    TestResult r = std::move(m_r);
    finish(r);
    for (int i = 0; i < int(std::size(kGroups)); ++i) {
        const auto it = m_groupStatus.find(i);
        if (it == m_groupStatus.end())
            continue;  // never started (Stop)
        r.subResults.append({QLatin1StringView(kGroups[i].id), it->second, m_groupReason[i], {}});
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
            firstProblem = QStringLiteral("%1: %2").arg(s.id, s.reason);
    }
    if (ok == int(std::size(kGroups))) {
        r.status = Status::Ok;
    } else if (ok > 0) {
        r.status = Status::Partial;
        r.statusReason = QStringLiteral("No CPU score: not every sub-test ran");
    } else {
        r.status = Status::Error;
        r.statusReason = firstProblem;
    }
    m_ctx.progress(1.0, QStringLiteral("Done"));
    return r;
}

} // namespace

CpuTest::CpuTest()
{
    m_info.id = QStringLiteral("cpu");
    m_info.name = QStringLiteral("CPU");
    m_info.summary = QStringLiteral("Integer, FP, SIMD, compression, encryption, sorting, physics");
    m_info.colorToken = QStringLiteral("t-cpu");
    // Both stay 0 until the anchors are frozen in calibration (task 18).
    m_info.testVersion = 0;
    m_info.formulaVersion = 0;
    for (const Group &g : kGroups)
        m_info.subTests.append({QLatin1StringView(g.id), QLatin1StringView(g.name), false, true});
}

Availability CpuTest::probe(const ProbeContext &) const
{
    Availability a;
    const isa::CpuFeatures f = isa::detect();
    using isa::Feature;
    switch (isa::bestSimdPath(f)) {
    case isa::SimdPath::Avx512:
        a.capabilities.append({QStringLiteral("AVX-512"), Tone::Good});
        break;
    case isa::SimdPath::Avx2Fma:
        a.capabilities.append({QStringLiteral("AVX2 + FMA"), Tone::Neutral});
        break;
    case isa::SimdPath::Sve:
        a.capabilities.append({QStringLiteral("SVE"), Tone::Good});
        break;
    case isa::SimdPath::Neon:
        a.capabilities.append({QStringLiteral("NEON"), Tone::Neutral});
        break;
    default:
        break;
    }
    if (f.has(Feature::Aes))
        a.capabilities.append({QStringLiteral("AES-NI"), Tone::Good});
    if (f.has(Feature::Sha))
        a.capabilities.append({QStringLiteral("SHA-NI"), Tone::Good});
    if (f.has(Feature::ArmAes) && f.has(Feature::ArmSha2))
        a.capabilities.append({QStringLiteral("ARMv8 Crypto"), Tone::Good});

    // Encryption needs its worker, and the worker must pass its self-test.
    QString cryptoProblem;
    if (cpu::CryptoClient::workerPath().isEmpty()) {
        cryptoProblem = QStringLiteral("The encryption worker (synbench-cpucrypto) is missing");
    } else {
        const cpu::CryptoOutcome self = cpu::CryptoClient::selfTest([] { return false; });
        if (!self.ok())
            cryptoProblem = QStringLiteral("The encryption worker failed its self-test: %1").arg(self.error);
    }
    if (!cryptoProblem.isEmpty()) {
        a.state = Availability::State::Partial;
        a.reason = cryptoProblem;
        a.subTestUnavailable.insert(QStringLiteral("crypto"), cryptoProblem);
    }

    // A warning, never Unavailable: 64 MiB plus 24 MiB per thread.
    const std::uint64_t need = (64ull + 24ull * std::uint64_t(cpu::logicalCpuCount())) << 20;
    if (const auto free = cpu::availableMemoryBytes(); free && *free < need)
        a.capabilities.append({QStringLiteral("Low free memory (%1 MiB, %2 MiB wanted)")
                                   .arg(*free >> 20)
                                   .arg(need >> 20),
                               Tone::Warn});
    return a;
}

OptionSchema CpuTest::options(const ProbeContext &, const Availability &availability) const
{
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
    info("threads", "Threads", QStringLiteral("All %1 for every sub-test").arg(cpu::logicalCpuCount()));
    const isa::CpuFeatures f = isa::detect();
    const isa::SimdPath path = isa::bestSimdPath(f);
    info("simd_path", "SIMD path",
         path == isa::SimdPath::None ? QStringLiteral("None") : QString::fromLatin1(isa::simdPathName(path)));
    if (availability.subTestUnavailable.contains(QStringLiteral("crypto")))
        info("encryption", "Encryption", availability.subTestUnavailable.value(QStringLiteral("crypto")), Tone::Warn);
    else
        info("encryption", "Encryption",
             QStringLiteral("Hardware AES + SHA (the software path is also measured, not scored)"));
    info("runs", "Runs", QStringLiteral("The CPU test always runs every part (about 4 min 30 s)"));
    return o;
}

std::chrono::seconds CpuTest::estimate(const OptionValues &) const
{
    return std::chrono::seconds(kBudgetSeconds);
}

std::chrono::seconds CpuTest::timeLimit(const OptionValues &o) const
{
    // 2 x the estimate + 60 s (§2), at the developer time scale for the
    // windows; the prep steps don't shrink with it, so never under the
    // floor below real time.
    const double scale = stubTimeScale();
    const double secs = 2.0 * double(estimate(o).count()) * scale + 60.0;
    const long long floor = scale < 1.0 ? stubTimeLimitFloor().count() : 0;
    return std::chrono::seconds(std::max<long long>(floor, std::llround(secs)));
}

TestResult CpuTest::run(RunContext &ctx, const OptionValues &)
{
    CpuRun run(ctx);
    return run.run();
}

ScoreBreakdown CpuTest::score(const TestResult &result) const
{
    return cpu::formula_v1::score(result);
}

} // namespace sb::tests
