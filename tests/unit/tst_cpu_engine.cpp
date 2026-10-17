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

#include <QTest>

#include <atomic>
#include <thread>

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

    // A kernel with a known cost: 1000 work per 2 ms unit, per thread.
    void knownCostRate()
    {
        const int n = testThreads();
        WorkerPool pool({n, {}});
        QCOMPARE(pool.threads(), n);
        const SpinKernel k([](int) { return 2ms; });
        // Windows long enough that a worker's wake-up latency (it counts
        // against the rate) stays far below the 5% tolerance.
        const Measurement m = pool.measure(k, {100ms, 600ms, 3}, [] { return false; });
        QVERIFY2(m.ok(), m.error.c_str());
        QCOMPARE(int(m.reps.size()), 3);
        const double expected = n * 1000.0 / 0.002;
        QVERIFY2(std::abs(m.median - expected) / expected < 0.05,
                 qPrintable(QStringLiteral("rate %1, expected %2").arg(m.median).arg(expected)));
        for (const RepSample &s : m.reps)
            QVERIFY(s.units > 0);
        QVERIFY(m.checksum != 0);
    }

    // P2: each thread's rate is its own work over its own elapsed time to the
    // end of its last unit, so a thread with long units isn't charged for the
    // part of its last unit that ran past the deadline, and nobody else is
    // either. Thread 0: 1 ms units; thread 1: 40 ms units.
    void unevenUnitsPerThreadAccounting()
    {
        if (logicalCpuCount() < 2)
            QSKIP("needs two CPUs");
        WorkerPool pool({2, {}});
        const SpinKernel k([](int t) { return t == 0 ? 1ms : 40ms; });
        const Measurement m = pool.measure(k, {50ms, 810ms, 3}, [] { return false; });
        QVERIFY2(m.ok(), m.error.c_str());
        const double expected = 1000.0 / 0.001 + 1000.0 / 0.040;
        QVERIFY2(std::abs(m.median - expected) / expected < 0.05,
                 qPrintable(QStringLiteral("rate %1, expected %2").arg(m.median).arg(expected)));
        // The slow thread finished its unit in progress at the deadline: a
        // repetition lasts up to one long unit longer than its window.
        for (const RepSample &s : m.reps)
            QVERIFY(s.seconds >= 0.80 && s.seconds < 0.90);
    }

    // Stop during a long repetition returns within 250 ms (§4.1), even with
    // 90 ms units.
    void stopWithin250ms()
    {
        WorkerPool pool({testThreads(), {}});
        const SpinKernel k([](int) { return 90ms; });
        std::atomic<bool> cancel{false};
        Clock::time_point cancelledAt{};
        std::thread canceller([&] {
            std::this_thread::sleep_for(400ms);
            cancelledAt = Clock::now();
            cancel = true;
        });
        const Measurement m = pool.measure(k, {100ms, 10000ms, 5}, [&] { return cancel.load(); });
        const auto returnedAt = Clock::now();
        canceller.join();
        QCOMPARE(m.outcome, Measurement::Outcome::Cancelled);
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(returnedAt - cancelledAt);
        QVERIFY2(took < 250ms, qPrintable(QStringLiteral("Stop took %1 ms").arg(took.count())));
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
        spreadOverProcessorGroups(0, 1);
    }
};

QTEST_GUILESS_MAIN(TestCpuEngine)
#include "tst_cpu_engine.moc"
