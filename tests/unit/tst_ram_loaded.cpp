// SPDX-License-Identifier: GPL-3.0-or-later
// The RAM test's loaded latency (Phase 3 design §5.4, §14.1 "Loaded"): the
// load threads leave out the chase core's SMT siblings and run where they
// are pinned; the delay calibration is monotone; every level reports a load
// bandwidth per repetition and a heavier load moves more bytes; Stop within
// 250 ms with the load running; every load thread joined on return. No
// absolute speed is asserted (CI runners are busy).
#include "isa/core_ranking.h"
#include "tests/cpu/engine/platform_threads.h"
#include "tests/cpu/engine/worker_pool.h"
#include "tests/ram/kernels/chase.h"
#include "tests/ram/kernels/cycle_builder.h"
#include "tests/ram/kernels/spin.h"
#include "tests/ram/loaded/load_generator.h"
#include "tests/ram/loaded/loaded_latency.h"
#include "tests/ram/memory/huge_buffer.h"

#include <QTest>

#include <atomic>
#include <chrono>
#include <thread>

using namespace sb::ram;
using namespace std::chrono_literals;
using sb::cpu::Clock;
using sb::isa::CoreChoice;

namespace {

constexpr std::size_t KiB = 1024, MiB = 1024 * KiB;

#if defined(__x86_64__) || defined(_M_X64)
const BwPath kPath = BwPath::Sse2;
#else
const BwPath kPath = BwPath::Neon;
#endif

// A chase on CPU 0 (its siblings unknown: just itself) and up to `loads`
// load threads on the next CPUs, each over its own 8 MiB of one buffer.
struct Rig {
    explicit Rig(int loads)
    {
        all = sb::isa::allowedLogicalCpus();
        if (all.size() < 2)
            return;
        chaseCore = all[0];
        const std::vector<CoreChoice> others = loadCpus(all, {sb::isa::cpuKey(chaseCore)});
        const std::size_t n = std::min<std::size_t>(std::size_t(loads), others.size());
        slices = HugeBuffer::allocate(n * 8 * MiB);
        slices.firstTouch({});
        for (std::size_t i = 0; i < n; ++i)
            specs.push_back({others[i], slices.data() + i * 8 * MiB, 8 * MiB});
        chaseBuffer = HugeBuffer::allocate(64 * MiB);
        chaseBuffer.firstTouch({});
        buildChaseCycle(chaseBuffer.data(), 64 * MiB, 64, chaseSeed(64 * MiB), {});
    }
    bool usable() const { return !specs.empty() && chaseBuffer.valid(); }

    std::vector<CoreChoice> all;
    CoreChoice chaseCore;
    HugeBuffer slices, chaseBuffer;
    std::vector<LoadThreadSpec> specs;
};

} // namespace

class TestRamLoaded : public QObject
{
    Q_OBJECT

private slots:
    // P12: the chase core's physical core (itself and its siblings) has no
    // load thread; everything else keeps its order.
    void loadCpusExcludeTheChaseCoresSiblings()
    {
        std::vector<CoreChoice> all;
        for (int c = 0; c < 32; ++c)
            all.push_back({true, c, 0, 0, "affinity"});
        const auto load = loadCpus(all, {9, 25});  // Tiamat's CPU 9 and its sibling
        QCOMPARE(load.size(), std::size_t(30));
        for (std::size_t i = 0; i + 1 < load.size(); ++i)
            QVERIFY(load[i].cpu < load[i + 1].cpu);
        for (const CoreChoice &c : load)
            QVERIFY(c.cpu != 9 && c.cpu != 25);
        // Windows keys: group 1, index 3 is key 67; group 0, index 3 stays.
        const std::vector<CoreChoice> groups = {{true, 3, 0, 0, ""}, {true, 3, 1, 0, ""}, {true, 4, 1, 0, ""}};
        const auto g = loadCpus(groups, {sb::isa::windowsKey(1, 3)});
        QCOMPARE(g.size(), std::size_t(2));
        QCOMPARE(g[0].group, 0);
        QCOMPARE(g[1].cpu, 4);
        // No siblings known: only the core itself is left out.
        QCOMPARE(loadCpus(all, {0}).size(), std::size_t(31));
    }

    void allowedLogicalCpusAreThisProcesss()
    {
        const auto cpus = sb::isa::allowedLogicalCpus();
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
        QVERIFY(!cpus.empty());
#endif
        QVERIFY(int(cpus.size()) <= sb::cpu::logicalCpuCount() || sb::cpu::logicalCpuCount() == 1);
#if defined(Q_OS_LINUX)
        QCOMPARE(int(cpus.size()), sb::cpu::logicalCpuCount());
#endif
        for (std::size_t i = 0; i + 1 < cpus.size(); ++i)
            QVERIFY(std::make_pair(cpus[i].group, cpus[i].cpu) < std::make_pair(cpus[i + 1].group, cpus[i + 1].cpu));
        for (const CoreChoice &c : cpus)
            QVERIFY(c.ranked);
    }

    // The load threads start where they are pinned, and are gone once the
    // generator is.
    void threadsPinnedAndJoined()
    {
        Rig rig(4);
        if (!rig.usable())
            QSKIP("needs two CPUs");
        {
            LoadGenerator load(kPath, rig.specs);
            QCOMPARE(load.threads(), int(rig.specs.size()));
            QCOMPARE(LoadGenerator::liveThreads(), int(rig.specs.size()));
            QCOMPARE(load.placementFailures(), 0);
            const std::vector<int> on = load.startedOn();
            for (std::size_t i = 0; i < rig.specs.size(); ++i) {
                if (on[i] >= 0 && rig.specs[i].cpu.group == 0)
                    QCOMPARE(on[i], rig.specs[i].cpu.cpu);
            }
            load.run(0);
            std::this_thread::sleep_for(20ms);
        }  // destroyed while running
        QCOMPARE(LoadGenerator::liveThreads(), 0);
    }

    // §5.4: more iterations take longer (and the calibration is positive).
    void delayCalibrationMonotone()
    {
        bool counted = true;
        const auto best = [&](std::uint64_t n) {
            double t = 1e300;
            for (int i = 0; i < 3; ++i) {
                const auto start = Clock::now();
                counted = counted && spinIterations(n) == n;
                t = std::min(t, std::chrono::duration<double, std::nano>(Clock::now() - start).count());
            }
            return t;
        };
        const double a = best(100000), b = best(1000000), c = best(10000000);
        QVERIFY(counted);
        QVERIFY(a < b);
        QVERIFY(b < c);
        Rig rig(1);
        if (!rig.usable())
            QSKIP("needs two CPUs");
        LoadGenerator load(kPath, rig.specs);
        const double ns = load.calibrate();
        QVERIFY(ns > 0);
        QVERIFY(ns < 100);  // even a slow, busy machine does an increment in under 100 ns
        QCOMPARE(load.nsPerIteration(), ns);
    }

    // Parked means no traffic; running means traffic; re-levelling keeps it.
    void parkStopsTheTraffic()
    {
        Rig rig(2);
        if (!rig.usable())
            QSKIP("needs two CPUs");
        LoadGenerator load(kPath, rig.specs);
        load.calibrate();
        const std::uint64_t idle = load.bytesRead();
        std::this_thread::sleep_for(20ms);
        QCOMPARE(load.bytesRead(), idle);
        load.run(0);
        std::this_thread::sleep_for(30ms);
        load.run(750);  // in place
        std::this_thread::sleep_for(30ms);
        load.park();
        const std::uint64_t moved = load.bytesRead();
        QVERIFY(moved > idle);
        std::this_thread::sleep_for(20ms);
        QCOMPARE(load.bytesRead(), moved);
    }

    // §5.4: five levels, each with a load bandwidth per repetition; the
    // full load moves more than the lightest; the chase measures throughout.
    void levelsHavePerRepetitionLoad()
    {
        Rig rig(3);
        if (!rig.usable())
            QSKIP("needs two CPUs");
        LoadGenerator load(kPath, rig.specs);
        sb::cpu::WorkerPool chasePool({1, rig.chaseCore});
        const ChaseKernel chase(rig.chaseBuffer.data(), 64 * MiB, 64, hopsPerUnit(100.0));
        std::vector<std::pair<int, int>> windows;
        const LoadedResult r = measureLoadedLatency(chasePool, chase, load, {20ms, 60ms, 3}, [] { return false; },
                                                    [&](int level, int window) { windows.push_back({level, window}); });
        QVERIFY2(r.ok(), r.error.c_str());
        QVERIFY(r.nsPerSpin > 0);
        QCOMPARE(r.levels.size(), std::size_t(5));
        QCOMPARE(windows.size(), std::size_t(5 * 4));
        QCOMPARE(windows.back(), std::make_pair(4, 3));
        for (std::size_t i = 0; i < r.levels.size(); ++i) {
            const LoadedLevel &l = r.levels[i];
            QCOMPARE(l.delayNs, kLoadDelaysNs[i]);
            QCOMPARE(l.repLoadGBs.size(), std::size_t(3));
            for (const double g : l.repLoadGBs)
                QVERIFY(g > 0);
            QVERIFY(l.latencyNs > 0);
            qInfo("delay %5.0f ns: chase %.1f ns, load %.2f GB/s (%d threads)", l.delayNs, l.latencyNs, l.loadGBs,
                  load.threads());
        }
        QVERIFY(r.levels.front().loadGBs > r.levels.back().loadGBs);
        // Parked on return.
        const std::uint64_t after = load.bytesRead();
        std::this_thread::sleep_for(20ms);
        QCOMPARE(load.bytesRead(), after);
    }

    // §14.1: Stop within 250 ms with the load running, and every load
    // thread joined once the generator goes.
    void stopWithin250msWithLoadRunning()
    {
        Rig rig(4);
        if (!rig.usable())
            QSKIP("needs two CPUs");
        Clock::time_point setAt{}, returned{};
        {
            LoadGenerator load(kPath, rig.specs);
            sb::cpu::WorkerPool chasePool({1, rig.chaseCore});
            const ChaseKernel chase(rig.chaseBuffer.data(), 64 * MiB, 64, hopsPerUnit(100.0));
            std::atomic<bool> cancel{false};
            std::thread stopper([&] {
                std::this_thread::sleep_for(300ms);  // in level 0's first repetition
                setAt = Clock::now();
                cancel = true;
            });
            const LoadedResult r = measureLoadedLatency(chasePool, chase, load, {100ms, 10s, 5},
                                                        [&] { return cancel.load(); });
            returned = Clock::now();
            stopper.join();
            QCOMPARE(r.outcome, sb::cpu::Measurement::Outcome::Cancelled);
            QVERIFY(r.levels.empty());
        }
        const auto joined = Clock::now();
        QVERIFY(returned - setAt < 250ms);
        QVERIFY(joined - setAt < 250ms);
        QCOMPARE(LoadGenerator::liveThreads(), 0);
    }

    void rejectsBadSlices()
    {
        alignas(64) static std::byte block[8192];
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, LoadGenerator(kPath, {{CoreChoice{}, block, 1000}}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, LoadGenerator(kPath, {{CoreChoice{}, nullptr, 4096}}));
        LoadGenerator none(kPath, {});
        QCOMPARE(none.calibrate(), 0.0);
    }
};

QTEST_GUILESS_MAIN(TestRamLoaded)
#include "tst_ram_loaded.moc"
