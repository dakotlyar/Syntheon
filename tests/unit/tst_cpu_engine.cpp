// SPDX-License-Identifier: GPL-3.0-or-later
// The CPU test's measurement engine (Phase 2 design §4, §6, §14.1 "Engine"):
// rates of a kernel with a known cost, per-thread elapsed accounting with
// uneven units, Stop within 250 ms, the pool joined on return, the
// plausibility ceiling, golden mismatches, pinning, opaque() and the dev time
// scale.
#include "tests/common/stub_clock.h"
#include "tests/cpu/engine/opaque.h"
#include "tests/cpu/engine/platform_threads.h"
#include "tests/cpu/engine/time_scale.h"
#include "tests/cpu/engine/worker_pool.h"

#include <QFile>
#include <QTest>

#if defined(Q_OS_LINUX)
#include <sched.h>
#endif


#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

using namespace sb::cpu;
using namespace std::chrono_literals;

namespace {

// Busy-waits a known time per unit (wall clock, so the rate doesn't depend
// on the machine's speed): thread t's units take cost(t).
class SpinKernel : public Kernel
{
public:
    explicit SpinKernel(std::function<Clock::duration(int thread)> cost, double work = 1000.0, double ceiling = 1e6)
        : m_cost(std::move(cost)), m_work(work), m_ceiling(ceiling)
    {
    }
    std::string name() const override { return "Spin"; }
    double workPerUnit() const override { return m_work; }
    double maxWorkPerCyclePerThread() const override { return m_ceiling; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override
    {
        struct T : KernelThread {
            Clock::duration cost;
            std::uint64_t unit(std::uint64_t index) override
            {
                const auto until = Clock::now() + cost;
                while (Clock::now() < until) {
                }
                return index * 0x9E3779B97F4A7C15ull;
            }
        };
        auto t = std::make_unique<T>();
        t->cost = m_cost(thread);
        return t;
    }
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override
    {
        if (m_wrongSeed && seed == *m_wrongSeed)
            return 0;  // the real checksum is never 0 for a seed > 0
        return std::nullopt;
    }
    std::optional<std::uint64_t> m_wrongSeed;

private:
    std::function<Clock::duration(int)> m_cost;
    double m_work;
    double m_ceiling;
};

// A fixed amount of CPU work: `count` steps of a dependent multiply-add
// chain, each through opaque() so none can be folded away.
std::uint64_t chain(std::uint64_t x, std::uint64_t count)
{
    for (std::uint64_t i = 0; i < count; ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        opaque(x);
    }
    return x;
}

// Steps of chain() that take about `d` on this machine, from the fastest of a
// few timed runs (the least disturbed one). Only sizes the units: no test
// result depends on it being accurate.
std::uint64_t chainStepsFor(Clock::duration d)
{
    constexpr std::uint64_t kProbe = 1u << 20;
    Clock::duration best = Clock::duration::max();
    for (int i = 0; i < 5; ++i) {
        const auto t0 = Clock::now();
        opaqueValue(chain(std::uint64_t(i), kProbe));
        best = std::min(best, Clock::now() - t0);
    }
    const double perStep = std::chrono::duration<double>(best).count() / double(kProbe);
    return std::max<std::uint64_t>(1, std::uint64_t(std::chrono::duration<double>(d).count() / perStep));
}

// Linux: how long thread `tid` (0 = the calling thread) has waited on a run
// queue, runnable but not running (the second field of schedstat), in ns.
// Elsewhere 0. For the Stop test: time the OS gave to other programs isn't
// the engine's.
std::uint64_t runQueueWaitNs(long tid = 0)
{
#if defined(Q_OS_LINUX)
    QFile f(tid ? QStringLiteral("/proc/self/task/%1/schedstat").arg(tid) : QStringLiteral("/proc/thread-self/schedstat"));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QList<QByteArray> fields = f.readAll().split(' ');
    return fields.size() >= 2 ? fields[1].trimmed().toULongLong() : 0;
#else
    (void)tid;
    return 0;
#endif
}

// A kernel whose units are a known amount of CPU work (thread t's units are
// steps(t) chain steps), and which records when each unit ran, tagged with the
// window it ran in (set by the measure() progress callback, which is called on
// the coordinator before a window's workers start and after the previous
// window's have all finished). The tests check the engine's accounting against
// these records, so their checks hold however the OS schedules the threads.
class RecordingKernel : public Kernel
{
public:
    struct Unit {
        int window;
        Clock::time_point start, end;
    };
    RecordingKernel(int threads, std::function<std::uint64_t(int thread)> steps)
        : m_steps(std::move(steps)), m_units(std::size_t(threads))
    {
    }
    std::string name() const override { return "Recording"; }
    double workPerUnit() const override { return 1000.0; }
    double maxWorkPerCyclePerThread() const override { return 1e6; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override
    {
        struct T : KernelThread {
            std::uint64_t steps;
            std::vector<Unit> *out;
            const std::atomic<int> *window;
            std::uint64_t unit(std::uint64_t index) override
            {
                Unit u{window->load(), Clock::now(), {}};
                const std::uint64_t sum = chain(index + 1, steps);
                u.end = Clock::now();
                out->push_back(u);
                return sum;
            }
        };
        auto t = std::make_unique<T>();
        t->steps = m_steps(thread);
        t->out = &m_units[std::size_t(thread)];
        t->window = &m_window;
        return t;
    }
    // For measure()'s progress callback (on the coordinator): the window
    // that starts now. Also notes the coordinator's run-queue wait so far.
    void startWindow(int window)
    {
        m_window = window;
        m_windowStarts.push_back(Clock::now());
        m_coordinatorWaits.push_back(runQueueWaitNs());
    }
    // After measure() returns, on the same thread: the last window's end.
    void finished() { m_coordinatorWaits.push_back(runQueueWaitNs()); }
    // The coordinator's run-queue wait during window `window` (Linux; else 0).
    double coordinatorWaitSeconds(int window) const
    {
        return double(m_coordinatorWaits[std::size_t(window) + 1] - m_coordinatorWaits[std::size_t(window)]) / 1e9;
    }
    // Read once measure() has returned (the pool's joins order the writes).
    const std::vector<Unit> &units(int thread) const { return m_units[std::size_t(thread)]; }
    Clock::time_point windowStart(int window) const { return m_windowStarts[std::size_t(window)]; }

private:
    std::function<std::uint64_t(int)> m_steps;
    mutable std::vector<std::vector<Unit>> m_units;
    std::atomic<int> m_window{-1};
    std::vector<Clock::time_point> m_windowStarts;
    std::vector<std::uint64_t> m_coordinatorWaits;
};

double secondsOf(Clock::duration d)
{
    return std::chrono::duration<double>(d).count();
}

// Two CPUs this process may pin to, as CoreChoices (Linux: the first two of
// the affinity mask; Windows: group 0's first two). Fewer when there aren't.
std::vector<sb::isa::CoreChoice> twoCpus()
{
    std::vector<sb::isa::CoreChoice> cpus;
#if defined(Q_OS_LINUX)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int c = 0; c < CPU_SETSIZE && cpus.size() < 2; ++c) {
            if (CPU_ISSET(c, &set))
                cpus.push_back({true, c, 0, 0, "test"});
        }
    }
#elif defined(Q_OS_WIN)
    for (int c = 0; c < std::min(2, logicalCpuCount()); ++c)
        cpus.push_back({true, c, 0, 0, "test"});
#endif
    return cpus;
}

// A kernel whose 1 ms units record the window they ran in and the CPU they
// ran on.
class WhereKernel : public Kernel
{
public:
    struct Unit {
        int window;
        int cpu;
    };
    std::string name() const override { return "Where"; }
    double workPerUnit() const override { return 1; }
    double maxWorkPerCyclePerThread() const override { return 1; }
    std::unique_ptr<KernelThread> makeThread(int) const override
    {
        struct T : KernelThread {
            const WhereKernel *k;
            std::uint64_t unit(std::uint64_t) override
            {
                std::this_thread::sleep_for(1ms);
                k->m_units.push_back({k->m_window.load(), currentCpu()});
                return 1;
            }
        };
        auto t = std::make_unique<T>();
        t->k = this;
        return t;
    }
    void startWindow(int window)
    {
        m_window = window;
        m_windowStarts.push_back(Clock::now());
    }
    // One thread only; read once measure() has returned.
    const std::vector<Unit> &units() const { return m_units; }
    const std::vector<Clock::time_point> &windowStarts() const { return m_windowStarts; }

private:
    mutable std::vector<Unit> m_units;
    std::atomic<int> m_window{-1};
    std::vector<Clock::time_point> m_windowStarts;
};

// Checks every repetition of `m` against the units `k` recorded (`threads`
// threads, repetitions of `window`). Empty if all hold, else what failed. Per
// repetition, from the records and the engine's stop time alone, with no
// assumption about how the OS scheduled the threads:
// - the unit count equals the engine's;
// - the rate lies in the bracket sum of W_t / E_t with E_t from the window's
//   start (between the progress callback and the first unit) to the end of
//   thread t's last unit, plus 1 ms for the engine's own stamp;
// - the repetition's length lies in the same bracket, and is at least the
//   window (nobody stops early);
// - per thread, at most one unit started after the coordinator set stop
//   (RepSample::stopSet): the one racing with the store; and stop was set
//   within the poll interval + 200 ms of the deadline.
QString checkRepetitions(const Measurement &m, const RecordingKernel &k, int threads, Clock::duration window)
{
    const int reps = int(m.reps.size());
    for (int r = 1; r <= reps; ++r) {
        const RepSample &s = m.reps[std::size_t(r - 1)];
        // The engine's window start lies between the progress callback and
        // the first unit of the window; E_t runs from it to the end of thread
        // t's last unit, which the engine stamps right after the kernel's own.
        const Clock::time_point earliest = k.windowStart(r);
        Clock::time_point latest = Clock::time_point::max();
        // When the coordinator set stop, as the engine recorded it (right after
        // the store). Every thread finishes the unit it is in; at most one
        // more can start, the one that raced with the store (its "stop?"
        // check came just before it). However the OS schedules the threads,
        // a thread that starts two units after this ignored stop.
        const Clock::time_point stopSet = s.stopSet;
        if (stopSet <= earliest)
            return QStringLiteral("repetition %1 has no stop time").arg(r);
        // ...and the coordinator set it at the deadline: within one poll
        // interval (50 ms) of it plus 200 ms, beyond the time it spent
        // waiting on a run queue in that window (Linux; a CPU other programs
        // hold delayed it by up to 380 ms beside 6 busy loops on 4 CPUs).
        const double late = secondsOf(stopSet - earliest) - secondsOf(window) - k.coordinatorWaitSeconds(r);
        if (late > 0.050 + 0.200)
            return QStringLiteral("repetition %1: stop was set %2 ms after the window started (window %3 ms; the "
                                  "coordinator waited %4 ms for a CPU)")
                .arg(r)
                .arg(secondsOf(stopSet - earliest) * 1000, 0, 'f', 1)
                .arg(secondsOf(window) * 1000, 0, 'f', 0)
                .arg(k.coordinatorWaitSeconds(r) * 1000, 0, 'f', 1);
        std::uint64_t units = 0;
        std::vector<Clock::time_point> ends;
        std::vector<std::uint64_t> counts;
        for (int t = 0; t < threads; ++t) {
            std::uint64_t n = 0;
            int startedAfterStop = 0;
            Clock::time_point end = earliest;
            for (const RecordingKernel::Unit &u : k.units(t)) {
                if (u.window != r)
                    continue;
                ++n;
                latest = std::min(latest, u.start);
                end = std::max(end, u.end);
                if (u.start > stopSet)
                    ++startedAfterStop;
            }
            if (n == 0)
                return QStringLiteral("thread %1 ran no unit in repetition %2").arg(t).arg(r);
            if (startedAfterStop > 1)
                return QStringLiteral("thread %1 started %2 units after stop was set in repetition %3")
                    .arg(t)
                    .arg(startedAfterStop)
                    .arg(r);
            units += n;
            ends.push_back(end);
            counts.push_back(n);
        }
        const double slack = 1e-3;  // the instructions between the kernel's stamp and the engine's
        const Clock::time_point slowestEnd = *std::max_element(ends.begin(), ends.end());
        double rateLow = 0, rateHigh = 0, chargedForSlowest = 0;
        for (int t = 0; t < threads; ++t) {
            const double work = double(counts[std::size_t(t)]) * k.workPerUnit();
            rateHigh += work / secondsOf(ends[std::size_t(t)] - latest);
            rateLow += work / (secondsOf(ends[std::size_t(t)] - earliest) + slack);
            chargedForSlowest += work / secondsOf(slowestEnd - earliest);
        }
        const double secondsLow = secondsOf(slowestEnd - latest);
        const double secondsHigh = secondsOf(slowestEnd - earliest) + slack;
        const QString detail = QStringLiteral("repetition %1: rate %2 (from the records: %3 to %4; charging every "
                                              "thread to the slowest one's end would give %5), %6 s (%7 to %8), "
                                              "%9 units (records: %10)")
                                   .arg(r)
                                   .arg(s.rate)
                                   .arg(rateLow)
                                   .arg(rateHigh)
                                   .arg(chargedForSlowest)
                                   .arg(s.seconds)
                                   .arg(secondsLow)
                                   .arg(secondsHigh)
                                   .arg(s.units)
                                   .arg(units);
        if (s.units != units || !(s.rate >= rateLow && s.rate <= rateHigh)
            || !(s.seconds >= secondsLow && s.seconds <= secondsHigh))
            return detail;
        // Nobody stops early: the thread in a unit at the deadline ends after it.
        if (s.seconds < secondsOf(window))
            return detail + QStringLiteral(": shorter than the window");
    }
    return {};
}

int testThreads()
{
    // Never more busy threads than CPUs, or wall-clock spinning slows down.
    return std::max(1, std::min(2, logicalCpuCount()));
}

} // namespace

class TestCpuEngine : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { sb::tests::setStubTimeScale(0); }

    void medianAndSpread()
    {
        QCOMPARE(medianOf({5, 1, 4, 2, 3}), 3.0);
        QCOMPARE(medianOf({4, 1, 3, 2}), 2.5);
        QCOMPARE(medianOf({}), 0.0);
        QCOMPARE(spreadOf({90, 100, 110, 100, 95}), 0.2);
        QCOMPARE(spreadOf({}), 0.0);
        Measurement m;
        m.spread = 0.11;
        QVERIFY(m.unstable());
        m.spread = 0.10;
        QVERIFY(!m.unstable());
    }

    // A kernel with a known cost per unit: about 2 ms of CPU work on every
    // thread, 1000 work each. The rate of each repetition is checked against
    // the kernel's own record of when its units ran (checkRepetitions), not
    // against the nominal 2 ms: on a loaded runner a unit can take far longer
    // than nominal (a 4-vCPU runner with every CPU busy once gave 934861
    // against 1e6 with a wall-clock spinning kernel), and the engine must
    // still account exactly what ran.
    void knownCostRate()
    {
        const int n = testThreads();
        RecordingKernel k(n, [steps = chainStepsFor(2ms)](int) { return steps; });
        WorkerPool pool({n, {}});
        QCOMPARE(pool.threads(), n);
        constexpr auto window = 600ms;
        const Measurement m = pool.measure(k, {100ms, window, 3}, [] { return false; }, [&](int w) { k.startWindow(w); });
        k.finished();
        QVERIFY2(m.ok(), m.error.c_str());
        QCOMPARE(int(m.reps.size()), 3);
        const QString problem = checkRepetitions(m, k, n, window);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        for (const RepSample &s : m.reps)
            QVERIFY(s.units > 0);
        QVERIFY(m.checksum != 0);
    }

    // P2: each thread's rate is its own work over its own elapsed time to the
    // end of its last unit, so a thread with long units isn't charged for the
    // part of its last unit that ran past the deadline, and nobody else is
    // either. Thread 0: units of about 1 ms of CPU work; thread 1: 40 times
    // that. Checked against the kernel's own record of when each unit ran, not
    // against nominal costs, so a runner that stalls a thread can't fail it.
    void unevenUnitsPerThreadAccounting()
    {
        if (logicalCpuCount() < 2)
            QSKIP("needs two CPUs");
        const std::uint64_t shortUnit = chainStepsFor(1ms);
        RecordingKernel k(2, [shortUnit](int t) { return t == 0 ? shortUnit : 40 * shortUnit; });
        WorkerPool pool({2, {}});
        constexpr auto window = 300ms;
        const Measurement m = pool.measure(k, {50ms, window, 3}, [] { return false; }, [&](int w) { k.startWindow(w); });
        k.finished();
        QVERIFY2(m.ok(), m.error.c_str());
        QCOMPARE(int(m.reps.size()), 3);
        const QString problem = checkRepetitions(m, k, 2, window);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
    }

    // Stop during a long repetition returns within 250 ms (§4.1): a poll
    // (50 ms), the units in progress finishing (30 ms of CPU work each), and
    // the engine's wrap-up. The cancel comes from inside a coordinator poll,
    // so it always waits one whole poll interval.
    //
    // On a runner whose CPUs other programs oversubscribe, threads also wait
    // on run queues (90 ms units took 370 ms of wall time 2x oversubscribed).
    // That is the load's, not the engine's. On Linux the coordinator reads
    // run-queue waits (schedstat) along the critical path: its own from the
    // cancel until it sees it, then the longest of its own and each worker's
    // from then until measure()'s return (they overlap: a sum would
    // over-subtract; workers' waits before it saw the cancel aren't on the
    // path). A
    // coordinator that polls too slowly sleeps and units that ignore the stop
    // work: neither is waiting, so both still fail. The poll itself is bound
    // too: seeing the cancel takes at most 50 ms + 75 ms beyond the
    // coordinator's own run-queue wait (timer granularity, and load delays
    // schedstat doesn't count: up to 100 ms beside 6 busy loops on 4 CPUs;
    // a 150 ms poll measured 153-180 ms idle, so it still fails). Elsewhere the waits read
    // as 0 and both bounds are on wall time (Windows raises the coordinator's
    // priority). On a failure the message splits the time.
    void stopWithin250ms()
    {
        const int n = testThreads();
        RecordingKernel k(n, [steps = chainStepsFor(30ms)](int) { return steps; });
        WorkerPool pool({n, {}});
        const std::vector<long> workers = pool.threadIds();
        const auto workerWaits = [&] {
            std::vector<std::uint64_t> w;
            for (const long tid : workers)
                w.push_back(runQueueWaitNs(tid));
            return w;
        };
        const Clock::time_point begin = Clock::now();
        Clock::time_point cancelledAt{}, seenAt{};
        bool armed = false;
        std::uint64_t coordAtCancel = 0, coordAtSeen = 0;
        std::vector<std::uint64_t> workersAtSeen;
        const Measurement m = pool.measure(
            k, {100ms, 10000ms, 5},
            [&] {
                const auto now = Clock::now();
                if (armed) {
                    if (seenAt == Clock::time_point{}) {
                        seenAt = now;
                        coordAtSeen = runQueueWaitNs();
                        workersAtSeen = workerWaits();
                    }
                    return true;
                }
                if (now - begin >= 300ms) {
                    armed = true;
                    cancelledAt = now;
                    coordAtCancel = runQueueWaitNs();
                }
                return false;
            },
            [&](int w) { k.startWindow(w); });
        const auto returnedAt = Clock::now();
        const std::uint64_t coordAtReturn = runQueueWaitNs();
        const std::vector<std::uint64_t> workersAtReturn = workerWaits();
        QCOMPARE(m.outcome, Measurement::Outcome::Cancelled);

        const double toSeenWait = double(coordAtSeen - coordAtCancel) / 1e6;
        double afterSeenWait = double(coordAtReturn - coordAtSeen) / 1e6;
        for (std::size_t t = 0; t < workers.size(); ++t)
            afterSeenWait = std::max(afterSeenWait, double(workersAtReturn[t] - workersAtSeen[t]) / 1e6);
        const double loadMs = toSeenWait + afterSeenWait;
        Clock::time_point lastEnd = seenAt;
        QStringList inFlight;
        for (int t = 0; t < n; ++t) {
            const RecordingKernel::Unit &u = k.units(t).back();
            lastEnd = std::max(lastEnd, u.end);
            inFlight << QStringLiteral("%1 ms").arg(secondsOf(u.end - u.start) * 1000, 0, 'f', 1);
        }
        const auto ms = [](Clock::duration d) { return QString::number(secondsOf(d) * 1000, 'f', 1); };
        const double tookMs = secondsOf(returnedAt - cancelledAt) * 1000;
        const double sawMs = secondsOf(seenAt - cancelledAt) * 1000;
        const QString detail =
            QStringLiteral("Stop took %1 ms, %2 ms of it other programs' (run-queue waits on the critical path: %3 "
                           "before the coordinator saw it, %4 after): the coordinator saw it after %5 ms (it polls "
                           "every 50 ms), the units in progress ended %6 ms later (their wall times: %7; 30 ms of "
                           "work), and measure() returned %8 ms after that")
                .arg(tookMs, 0, 'f', 1)
                .arg(loadMs, 0, 'f', 1)
                .arg(toSeenWait, 0, 'f', 1)
                .arg(afterSeenWait, 0, 'f', 1)
                .arg(ms(seenAt - cancelledAt))
                .arg(ms(lastEnd - seenAt))
                .arg(inFlight.join(QStringLiteral(", ")))
                .arg(ms(returnedAt - lastEnd));
        QVERIFY2(sawMs - toSeenWait < 50 + 75, qPrintable(QStringLiteral("the poll is too slow. ") + detail));
        QVERIFY2(tookMs - loadMs < 250, qPrintable(detail));
    }

    // Phase 1 threading rule 4: the workers are gone when the pool is.
    void poolJoinedOnReturn()
    {
        const int before = WorkerPool::liveThreads();
        {
            WorkerPool pool({3, {}});
            QCOMPARE(WorkerPool::liveThreads(), before + 3);
            const SpinKernel k([](int) { return 1ms; });
            QVERIFY(pool.measure(k, {10ms, 10ms, 1}, [] { return false; }).ok());
        }
        QCOMPARE(WorkerPool::liveThreads(), before);
    }

    // P14: a rate no CPU can reach (the work was "optimised away") is an Error.
    void plausibilityCeilingIsAnError()
    {
        QCOMPARE(plausibilityCeiling(8, 24), 8 * 7e9 * 24 * 1.5);
        WorkerPool pool({1, {}});
        // 1e12 work per 1 ms unit is 1e15/s; the ceiling for 1 per cycle is 1.05e10/s.
        const SpinKernel k([](int) { return 1ms; }, 1e12, 1.0);
        const Measurement m = pool.measure(k, {10ms, 30ms, 3}, [] { return false; });
        QCOMPARE(m.outcome, Measurement::Outcome::Implausible);
        QVERIFY(QString::fromStdString(m.error).contains(QLatin1StringView("implausibly fast")));
        QCOMPARE(int(m.reps.size()), 3);  // the measured values are still there
    }

    // §6.1: a golden mismatch ends the measurement with an Error naming the seed.
    void goldenMismatchIsAnError()
    {
        WorkerPool pool({testThreads(), {}});
        SpinKernel k([](int) { return 1ms; });
        k.m_wrongSeed = 17;
        const Measurement m = pool.measure(k, {200ms, 200ms, 5}, [] { return false; });
        QCOMPARE(m.outcome, Measurement::Outcome::WrongResult);
        QCOMPARE(QString::fromStdString(m.error),
                 QStringLiteral("Spin kernel produced a wrong result (seed 17): possible miscompile or hardware fault"));
        // The pool is still usable for the next sub-test.
        k.m_wrongSeed.reset();
        QVERIFY(pool.measure(k, {10ms, 20ms, 1}, [] { return false; }).ok());
    }

    void kernelExceptionIsAnError()
    {
        struct Throwing : SpinKernel {
            Throwing() : SpinKernel([](int) { return 1ms; }) {}
            std::unique_ptr<KernelThread> makeThread(int) const override { throw std::bad_alloc(); }
        };
        WorkerPool pool({2, {}});
        const Measurement m = pool.measure(Throwing(), {10ms, 10ms, 1}, [] { return false; });
        QCOMPARE(m.outcome, Measurement::Outcome::Failed);
        QVERIFY(QString::fromStdString(m.error).startsWith(QLatin1StringView("Spin: ")));
    }

    // AP-5 M5: every thread goes through every seed, also with 64 threads
    // (interleaved indices gave each thread one seed there). Driven by unit
    // counts, not time: the measurement is stopped (as a cancel) once every
    // thread has done 64 units, however slowly a loaded runner schedules 64
    // threads.
    void everyThreadSeesEverySeed()
    {
        constexpr int kThreads = 64;
        struct SeedKernel : Kernel {
            mutable std::mutex mutex;
            mutable std::vector<std::set<std::uint64_t>> seen;
            mutable std::vector<std::atomic<int>> units;
            SeedKernel() : seen(kThreads), units(kThreads) {}
            std::string name() const override { return "Seeds"; }
            double workPerUnit() const override { return 1; }
            double maxWorkPerCyclePerThread() const override { return 1e9; }
            std::unique_ptr<KernelThread> makeThread(int thread) const override
            {
                struct T : KernelThread {
                    std::set<std::uint64_t> local;
                    std::set<std::uint64_t> *out;
                    std::atomic<int> *count;
                    std::mutex *mutex;
                    ~T() override
                    {
                        std::lock_guard lock(*mutex);
                        out->insert(local.begin(), local.end());
                    }
                    std::uint64_t unit(std::uint64_t index) override
                    {
                        local.insert(index % kSeeds);
                        ++*count;
                        return index;
                    }
                };
                auto t = std::make_unique<T>();
                std::lock_guard lock(mutex);
                t->out = &seen[std::size_t(thread)];
                t->count = &units[std::size_t(thread)];
                t->mutex = &mutex;
                return t;
            }
            bool allDone() const
            {
                for (const std::atomic<int> &u : units) {
                    if (u.load() < int(kSeeds))
                        return false;
                }
                return true;
            }
        } k;
        WorkerPool pool({kThreads, {}});
        // A generous time limit only as a backstop; the stop comes from the counts.
        const Measurement m = pool.measure(k, {60000ms, 60000ms, 1}, [&k] { return k.allDone(); });
        QCOMPARE(m.outcome, Measurement::Outcome::Cancelled);
        QVERIFY(k.allDone());
        for (const std::set<std::uint64_t> &s : k.seen)
            QCOMPARE(int(s.size()), int(kSeeds));
    }

    // Kernel::seeds(): a kernel with 128 inputs (the compression kernels'
    // corpus blocks) gets its golden table asked for seeds 0..127, each
    // thread goes through all of them, and a mismatch names the seed.
    // Driven by unit counts, like everyThreadSeesEverySeed.
    void kernelWithMoreSeeds()
    {
        constexpr int kThreads = 3;
        struct Blocks : Kernel {
            mutable std::mutex mutex;
            mutable std::set<std::uint64_t> asked;
            mutable std::vector<std::atomic<int>> units;
            std::optional<std::uint64_t> wrong;
            Blocks() : units(kThreads) {}
            std::string name() const override { return "Blocks"; }
            double workPerUnit() const override { return 1; }
            double maxWorkPerCyclePerThread() const override { return 1e9; }
            std::uint64_t seeds() const override { return 128; }
            std::unique_ptr<KernelThread> makeThread(int thread) const override
            {
                struct T : KernelThread {
                    std::atomic<int> *count;
                    std::uint64_t unit(std::uint64_t index) override
                    {
                        ++*count;
                        return index % 128 + 1;
                    }
                };
                auto t = std::make_unique<T>();
                t->count = &units[std::size_t(thread)];
                return t;
            }
            std::optional<std::uint64_t> golden(std::uint64_t seed) const override
            {
                std::lock_guard lock(mutex);
                asked.insert(seed);
                return wrong && seed == *wrong ? std::optional<std::uint64_t>(0) : std::optional(seed + 1);
            }
            bool allDone() const
            {
                for (const std::atomic<int> &u : units) {
                    if (u.load() < 128)
                        return false;
                }
                return true;
            }
        } k;
        WorkerPool pool({kThreads, {}});
        const Measurement m = pool.measure(k, {60000ms, 60000ms, 1}, [&k] { return k.allDone(); });
        QCOMPARE(m.outcome, Measurement::Outcome::Cancelled);
        QCOMPARE(int(k.asked.size()), 128);
        QCOMPARE(*k.asked.rbegin(), std::uint64_t(127));
        k.wrong = 100;
        const Measurement bad = pool.measure(k, {60000ms, 60000ms, 1}, [] { return false; });
        QCOMPARE(bad.outcome, Measurement::Outcome::WrongResult);
        QVERIFY(QString::fromStdString(bad.error).contains(QLatin1StringView("(seed 100)")));
    }

    // KernelThread::cancelRequested(): a unit far longer than 100 ms (a codec
    // call on a slow CPU) that polls it ends at a Stop within 250 ms, and its
    // early-returned result is neither counted nor checked (the golden here
    // would call it wrong).
    void cancelReachesALongUnit()
    {
        struct Long : Kernel {
            std::string name() const override { return "Long"; }
            double workPerUnit() const override { return 1; }
            double maxWorkPerCyclePerThread() const override { return 1e9; }
            std::unique_ptr<KernelThread> makeThread(int) const override
            {
                struct T : KernelThread {
                    std::uint64_t unit(std::uint64_t) override
                    {
                        // 10 s of "work" in 1 ms steps, unless cancelled.
                        const auto until = Clock::now() + 10s;
                        while (Clock::now() < until && !cancelRequested())
                            std::this_thread::sleep_for(1ms);
                        return 0;
                    }
                };
                return std::make_unique<T>();
            }
            std::optional<std::uint64_t> golden(std::uint64_t) const override { return 1; }
        } k;
        WorkerPool pool({testThreads(), {}});
        std::atomic<bool> cancel{false};
        Clock::time_point cancelledAt{};
        std::thread canceller([&] {
            std::this_thread::sleep_for(300ms);
            cancelledAt = Clock::now();
            cancel = true;
        });
        const Measurement m = pool.measure(k, {100ms, 10000ms, 5}, [&] { return cancel.load(); });
        const auto took = Clock::now() - cancelledAt;
        canceller.join();
        QCOMPARE(m.outcome, Measurement::Outcome::Cancelled);
        QVERIFY2(took < 250ms, qPrintable(QStringLiteral("Stop took %1 ms").arg(secondsOf(took) * 1000)));
        QVERIFY(m.error.empty());
    }

    // KernelThread::windowRate(): a kernel's own rate for a window replaces
    // W_t / E_t, summed over threads; beginWindow() comes before each window.
    void kernelTimedWindowRate()
    {
        struct Own : Kernel {
            mutable std::atomic<int> windows{0};
            std::string name() const override { return "Own"; }
            double workPerUnit() const override { return 1e6; }
            double maxWorkPerCyclePerThread() const override { return 1e9; }
            std::unique_ptr<KernelThread> makeThread(int) const override
            {
                struct T : KernelThread {
                    std::atomic<int> *windows;
                    bool begun = false;
                    void beginWindow() override
                    {
                        begun = true;
                        ++*windows;
                    }
                    std::optional<double> windowRate() const override { return begun ? 1234.5 : -1.0; }
                    std::uint64_t unit(std::uint64_t) override
                    {
                        std::this_thread::sleep_for(1ms);
                        return 1;
                    }
                };
                auto t = std::make_unique<T>();
                t->windows = &windows;
                return t;
            }
        } k;
        WorkerPool pool({2, {}});
        const Measurement m = pool.measure(k, {10ms, 20ms, 3}, [] { return false; });
        QVERIFY2(m.ok(), m.error.c_str());
        QCOMPARE(m.median, 2 * 1234.5);
        QCOMPARE(k.windows.load(), 2 * 4);  // warm-up + 3 repetitions, two threads
    }

    // Kernel::collectiveThreads(): all threads run the same units together
    // (they meet at the kernel's own barrier inside each unit), the stop
    // decision is the same for all of them, each is credited 1/N of a unit,
    // and a pool of another size fails the measurement.
    void collectiveKernel()
    {
        constexpr int kThreads = 3;
        struct Together : Kernel {
            mutable std::barrier<> meet{kThreads};
            mutable std::vector<std::vector<std::uint64_t>> seen = std::vector<std::vector<std::uint64_t>>(kThreads);
            std::string name() const override { return "Together"; }
            double workPerUnit() const override { return 3000; }
            double maxWorkPerCyclePerThread() const override { return 1e9; }
            int collectiveThreads() const override { return kThreads; }
            std::unique_ptr<KernelThread> makeThread(int thread) const override
            {
                struct T : KernelThread {
                    std::barrier<> *meet;
                    std::vector<std::uint64_t> *seen;
                    std::uint64_t unit(std::uint64_t index) override
                    {
                        seen->push_back(index);
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                        meet->arrive_and_wait();  // nobody finishes a unit alone
                        return index % kSeeds + 7;
                    }
                };
                auto t = std::make_unique<T>();
                t->meet = &meet;
                t->seen = &seen[std::size_t(thread)];
                return t;
            }
            std::optional<std::uint64_t> golden(std::uint64_t seed) const override { return seed + 7; }
        } k;
        WorkerPool pool({kThreads, {}});
        const Measurement m = pool.measure(k, {20ms, 60ms, 3}, [] { return false; });
        QVERIFY2(m.ok(), m.error.c_str());
        // Every thread ran exactly the same units, in the same order.
        QVERIFY(!k.seen[0].empty());
        QCOMPARE(k.seen[1], k.seen[0]);
        QCOMPARE(k.seen[2], k.seen[0]);
        for (std::size_t i = 0; i < k.seen[0].size(); ++i)
            QCOMPARE(k.seen[0][i], std::uint64_t(i));
        // Each repetition counts each shared unit once (units and rate), and
        // every thread ran all of them.
        std::uint64_t total = 0;
        for (const RepSample &r : m.reps) {
            QVERIFY(r.units > 0);
            // Against the repetition's own length, not the window: a thread
            // stamps its last unit's end before the stop barrier, so on a
            // loaded CPU its E_t can be shorter than the window. The threads'
            // E_t differ only by the barrier's wake-up skew; counting each
            // shared unit 3 times would be 3 times this (and N/(N-1) = 1.5 is
            // what a wrong per-thread split would give: the bound stays under).
            const double perShared = double(r.units) * 3000 / r.seconds;
            QVERIFY2(r.rate < perShared * 1.4 && r.rate >= perShared * 0.6,
                     qPrintable(QStringLiteral("rate %1, %2 shared units in %3 s").arg(r.rate).arg(r.units).arg(r.seconds)));
            total += r.units;
        }
        QVERIFY(total < k.seen[0].size());  // the warm-up's units come first
        // A cancel ends all of them together.
        std::atomic<int> polls{0};
        QCOMPARE(pool.measure(k, {10000ms, 10000ms, 1}, [&] { return ++polls > 3; }).outcome,
                 Measurement::Outcome::Cancelled);
        QCOMPARE(k.seen[1].size(), k.seen[0].size());
        QCOMPARE(k.seen[2].size(), k.seen[0].size());
        // The wrong pool size.
        WorkerPool two({2, {}});
        const Measurement wrong = two.measure(k, {10ms, 10ms, 1}, [] { return false; });
        QCOMPARE(wrong.outcome, Measurement::Outcome::Failed);
        QCOMPARE(QString::fromStdString(wrong.error), QStringLiteral("Together: built for 3 threads, the pool has 2"));
    }

    // §6.1: the sink doesn't cancel when the same checksum repeats (it does
    // for every kernel, once per 64 seeds; an XOR fold gave 0 for an even count).
    void checksumSinkDoesNotCancel()
    {
        struct Same : Kernel {
            std::string name() const override { return "Same"; }
            double workPerUnit() const override { return 1; }
            double maxWorkPerCyclePerThread() const override { return 1e9; }
            std::unique_ptr<KernelThread> makeThread(int) const override
            {
                struct T : KernelThread {
                    std::uint64_t unit(std::uint64_t) override { return 0x1234; }
                };
                return std::make_unique<T>();
            }
        } k;
        WorkerPool pool({2, {}});
        for (int i = 0; i < 5; ++i)
            QVERIFY(pool.measure(k, {5ms, 10ms, 2}, [] { return false; }).checksum != 0);
    }

    void progressPerWindow()
    {
        WorkerPool pool({1, {}});
        const SpinKernel k([](int) { return 1ms; });
        std::vector<int> windows;
        pool.measure(k, {10ms, 10ms, 3}, [] { return false; }, [&](int w) { windows.push_back(w); });
        QCOMPARE(windows, (std::vector<int>{0, 1, 2, 3}));
    }

    // P4: the single-thread pool runs pinned on the chosen core.
    void singleThreadPinned()
    {
        sb::isa::CoreChoice core = sb::isa::bestCoreForSingleThread();
        if (!core.ranked) {
            // No ranking data here (a VM): pin to the CPU we're on, to test the mechanism.
            core.ranked = currentCpu() >= 0;
            core.cpu = currentCpu();
        }
        if (!core.ranked)
            QSKIP("no CPU number on this platform");
        WorkerPool pool({1, core});
        QVERIFY(pool.pinned());
        // Every unit checks the CPU it runs on.
        std::atomic<int> wrongCpu{0};
        struct CheckKernel : Kernel {
            int want;
            std::atomic<int> *wrong;
            std::string name() const override { return "Check"; }
            double workPerUnit() const override { return 1; }
            double maxWorkPerCyclePerThread() const override { return 1; }
            std::unique_ptr<KernelThread> makeThread(int) const override
            {
                struct T : KernelThread {
                    int want;
                    std::atomic<int> *wrong;
                    std::uint64_t unit(std::uint64_t) override
                    {
                        std::this_thread::sleep_for(1ms);
                        if (currentCpu() != want)
                            ++*wrong;
                        return 1;
                    }
                };
                auto t = std::make_unique<T>();
                t->want = want;
                t->wrong = wrong;
                return t;
            }
        } check;
        check.want = core.cpu;
        check.wrong = &wrongCpu;
        QVERIFY(pool.measure(check, {10ms, 100ms, 1}, [] { return false; }).ok());
        QCOMPARE(wrongCpu.load(), 0);
    }

    // Decision D: before each repetition (not the warm-up) the pool samples
    // idle time for 250 ms, outside the window, and re-pins to the quieter
    // candidate; each repetition records where it ran. Injected samples make
    // the second CPU the quieter one in repetitions 1 and 3, the first in 2.
    void repickPerRepetition()
    {
        const std::vector<sb::isa::CoreChoice> cpus = twoCpus();
        if (cpus.size() < 2)
            QSKIP("needs two CPUs to pin to");
        WorkerPool::Options o;
        o.threads = 1;
        o.pinTo = cpus[0];
        o.pickFrom = {{cpus[0], {100}}, {cpus[1], {200}}};
        std::vector<Clock::time_point> sampledAt;
        o.idleSampler = [&] {
            const int call = int(sampledAt.size());
            sampledAt.push_back(Clock::now());
            const int rep = call / 2 + 1;
            const bool second = call % 2 == 1;
            const std::uint64_t quiet = second ? 25 : 0, busy = second ? 5 : 0;
            return rep == 2 ? sb::isa::IdleSample{{100, quiet}, {200, busy}}
                            : sb::isa::IdleSample{{100, busy}, {200, quiet}};
        };
        WorkerPool pool(std::move(o));
        QVERIFY(pool.pinned());
        WhereKernel k;
        const Measurement m = pool.measure(k, {20ms, 100ms, 3}, [] { return false; },
                                           [&](int w) { k.startWindow(w); });
        QVERIFY(m.ok());
        QCOMPARE(sampledAt.size(), std::size_t(6));  // two per repetition, none for the warm-up
        QCOMPARE(m.reps.size(), std::size_t(3));
        const int want[] = {cpus[0].cpu, cpus[1].cpu, cpus[0].cpu, cpus[1].cpu};  // warm-up = pinTo
        for (int r = 1; r <= 3; ++r) {
            QVERIFY(m.reps[std::size_t(r - 1)].core.ranked);
            QCOMPARE(m.reps[std::size_t(r - 1)].core.cpu, want[r]);
            // 250 ms of sampling, all before the window started...
            const auto first = sampledAt[std::size_t(2 * r - 2)], second = sampledAt[std::size_t(2 * r - 1)];
            QVERIFY(second - first >= 250ms);
            QVERIFY(k.windowStarts()[std::size_t(r)] >= second);
            // ...so the repetition's own time doesn't include it.
            QVERIFY2(m.reps[std::size_t(r - 1)].seconds < 0.100 + 0.150,
                     qPrintable(QString::number(m.reps[std::size_t(r - 1)].seconds)));
        }
        int wrong = 0;
        for (const WhereKernel::Unit &u : k.units()) {
            if (currentCpu() >= 0 && u.cpu != want[u.window])
                ++wrong;
        }
        QCOMPARE(wrong, 0);
        QCOMPARE(pool.placementFailures(), 0);
    }

    // One candidate: nothing to choose, so no sampling.
    void singleCandidateNoSampling()
    {
        const std::vector<sb::isa::CoreChoice> cpus = twoCpus();
        if (cpus.empty())
            QSKIP("needs a CPU to pin to");
        WorkerPool::Options o;
        o.threads = 1;
        o.pinTo = cpus[0];
        o.pickFrom = {{cpus[0], {0}}};
        int samples = 0;
        o.idleSampler = [&] {
            ++samples;
            return sb::isa::IdleSample{};
        };
        WorkerPool pool(std::move(o));
        WhereKernel k;
        const auto start = Clock::now();
        const Measurement m = pool.measure(k, {10ms, 20ms, 3}, [] { return false; });
        QVERIFY(m.ok());
        QCOMPARE(samples, 0);
        QVERIFY(Clock::now() - start < 250ms * 3);
        for (const RepSample &s : m.reps)
            QCOMPARE(s.core.cpu, cpus[0].cpu);
    }

    // Stop during the sampling still returns within 250 ms: it polls cancel
    // every 50 ms, like a window (a 10 s sample shows it doesn't wait for the
    // end). On Linux the coordinator's run-queue wait is subtracted, as in
    // stopWithin250ms.
    void stopDuringSampling()
    {
        const std::vector<sb::isa::CoreChoice> cpus = twoCpus();
        if (cpus.size() < 2)
            QSKIP("needs two CPUs to pin to");
        WorkerPool::Options o;
        o.threads = 1;
        o.pinTo = cpus[0];
        o.pickFrom = {{cpus[0], {0}}, {cpus[1], {1}}};
        o.sampleFor = 10000ms;
        int samples = 0;
        Clock::time_point cancelledAt{};
        std::uint64_t wait0 = 0;
        o.idleSampler = [&] {
            // Repetition 1's first sample: cancel 100 ms into the sampling.
            if (++samples == 1) {
                cancelledAt = Clock::now() + 100ms;
                wait0 = runQueueWaitNs();
            }
            return sb::isa::IdleSample{};
        };
        WorkerPool pool(std::move(o));
        WhereKernel k;
        const Measurement m = pool.measure(
            k, {10ms, 20ms, 3},
            [&] { return cancelledAt != Clock::time_point{} && Clock::now() >= cancelledAt; });
        const auto returnedAt = Clock::now();
        const double waitMs = double(runQueueWaitNs() - wait0) / 1e6;
        QCOMPARE(m.outcome, Measurement::Outcome::Cancelled);
        QCOMPARE(samples, 1);
        QVERIFY(m.reps.empty());
        const double tookMs = secondsOf(returnedAt - cancelledAt) * 1000;
        QVERIFY2(tookMs - waitMs < 250, qPrintable(QStringLiteral("Stop took %1 ms (%2 ms waiting for a CPU)")
                                                       .arg(tookMs, 0, 'f', 1)
                                                       .arg(waitMs, 0, 'f', 1)));
    }

    void unrankedCoreRunsUnpinned()
    {
        WorkerPool pool({1, {}});
        QVERIFY(!pool.pinned());
    }

    // opaque() keeps values and pointers intact (what it hides is checked by
    // the kernels' golden tables and the plausibility ceiling).
    void opaqueKeepsValues()
    {
        int i = 42;
        double d = 1.5;
        const char *p = "x";
        opaque(i);
        opaque(d);
        opaque(p);
        QCOMPARE(i, 42);
        QCOMPARE(d, 1.5);
        QCOMPARE(*p, 'x');
        QCOMPARE(opaqueValue(std::uint64_t(7)), std::uint64_t(7));
    }

    void devTimeScale()
    {
        sb::tests::setStubTimeScale(0.5);
        QCOMPARE(scaledWindow(4000ms), 2000ms);
        sb::tests::setStubTimeScale(0.001);
        QCOMPARE(scaledWindow(4000ms), 10ms);  // the floor
        const WindowSpec s = scaled({2000ms, 4000ms, 5});
        QCOMPARE(s.warmup, 10ms);
        QCOMPARE(s.reps, 5);
        sb::tests::setStubTimeScale(1.0);
        QCOMPARE(scaledWindow(3000ms), 3000ms);
    }

    void logicalCpus()
    {
        QVERIFY(logicalCpuCount() >= 1);
        WorkerPool pool({});
        QCOMPARE(pool.threads(), logicalCpuCount());
    }

    // P5: Windows only; elsewhere these do nothing and must not fail.
    void powerThrottlingOptOut()
    {
        ScopedProcessPowerThrottlingOptOut scope;
#if defined(Q_OS_WIN)
        QVERIFY(scope.active());
#else
        QVERIFY(!scope.active());
#endif
        optOutOfPowerThrottlingForThisThread();
        QVERIFY(spreadOverProcessorGroups(0, 1));
    }
};

QTEST_GUILESS_MAIN(TestCpuEngine)
#include "tst_cpu_engine.moc"
