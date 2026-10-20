// SPDX-License-Identifier: GPL-3.0-or-later
// The RAM test's pointer chase (Phase 3 design §5.1, §14.1 "Cycle builder",
// "Chase"): Sattolo gives one cycle through every line for every grid size
// up to 64 MiB and once for 1 GiB (full walks); the same seed gives the same
// order on every platform (pinned hashes); a cancel returns within 250 ms
// during a 2 GiB build; n hops = an independent reference walk; the range
// check fires on planted bad pointers; K per unit is constant and sized as
// §5.1 says; and a real measurement reads DRAM slower than L1.
#include "isa/core_ranking.h"
#include "tests/cpu/engine/platform_threads.h"
#include "tests/cpu/engine/worker_pool.h"
#include "tests/ram/kernels/chase.h"
#include "tests/ram/kernels/cycle_builder.h"
#include "tests/ram/memory/huge_buffer.h"
#include "tests/ram/sweep/grid.h"

#include <QTest>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace sb::ram;
using namespace std::chrono_literals;
using sb::cpu::Clock;

namespace {

constexpr std::size_t KiB = 1024, MiB = 1024 * KiB, GiB = 1024 * MiB;
constexpr std::size_t kLine = 64;

// FNV-1a over the index array's values (little-endian bytes).
std::uint64_t fnv1a(const std::vector<std::uint32_t> &v)
{
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const std::uint32_t x : v) {
        for (int b = 0; b < 4; ++b) {
            h ^= (x >> (8 * b)) & 0xFFu;
            h *= 0x100000001B3ull;
        }
    }
    return h;
}

// Walks the cycle from `base`; the number of hops until it is back at the
// start, or 0 if it leaves the buffer, lands off a line start, or doesn't
// come back within the line count (then it visited some line twice).
std::size_t cycleLength(const std::byte *base, std::size_t bytes)
{
    const std::size_t lines = bytes / kLine;
    const void *p = base;
    for (std::size_t hop = 1; hop <= lines; ++hop) {
        p = *static_cast<const void *const *>(p);
        if (!chaseInRange(p, base, bytes, kLine))
            return 0;
        if (p == base)
            return hop;
    }
    return 0;
}

HugeBuffer touchedBuffer(std::size_t bytes)
{
    HugeBuffer b = HugeBuffer::allocate(bytes);
    if (b.valid())
        b.firstTouch({});
    return b;
}

} // namespace

class TestRamChase : public QObject
{
    Q_OBJECT

private slots:
    // Every grid size up to 64 MiB: one cycle through all S / 64 lines.
    void everyGridSizeIsOneFullCycle()
    {
        HugeBuffer buf = touchedBuffer(64 * MiB);
        QVERIFY2(buf.valid(), buf.error().c_str());
        for (const std::uint64_t size : latencyGrid(64 * MiB)) {
            QCOMPARE(buildChaseCycle(buf.data(), size, kLine, chaseSeed(size), {}), CycleBuild::Ok);
            QCOMPARE(cycleLength(buf.data(), size), std::size_t(size / kLine));
        }
    }

    // ...and once at 1 GiB (16 Mi lines).
    void oneGibIsOneFullCycle()
    {
        HugeBuffer buf = touchedBuffer(GiB);
        if (!buf.valid())
            QSKIP("no 1 GiB for the full walk on this machine");
        QCOMPARE(buildChaseCycle(buf.data(), GiB, kLine, chaseSeed(GiB), {}), CycleBuild::Ok);
        QCOMPARE(cycleLength(buf.data(), GiB), GiB / kLine);
    }

    // A 128 B line (some ARM cores) links 128 B apart: half the lines.
    void wideLinesLinkWholeLines()
    {
        std::vector<std::uint64_t> mem(64 * KiB / 8);
        auto *base = reinterpret_cast<std::byte *>(mem.data());
        QCOMPARE(buildChaseCycle(base, 64 * KiB, 128, chaseSeed(64 * KiB), {}), CycleBuild::Ok);
        const void *p = base;
        std::size_t hops = 0;
        do {
            p = *static_cast<const void *const *>(p);
            QVERIFY(chaseInRange(p, base, 64 * KiB, 128));
            ++hops;
        } while (p != base && hops <= 512);
        QCOMPARE(hops, std::size_t(512));
    }

    // P3: the order is a function of the seed alone (xoshiro256**, no
    // standard-library engine): these hashes hold on every OS and compiler.
    // They come from an independent Python transcription of splitmix64,
    // xoshiro256**, Lemire's bounded draw, Sattolo and FNV-1a, not from
    // this code's own output.
    void sameSeedSameOrderEverywhere()
    {
        struct Case {
            std::size_t bytes;
            std::uint64_t hash;
        };
        const Case cases[] = {
            {4 * KiB, 0x48062e654de21695ull},
            {1 * MiB, 0xa1d8cc5d0200c531ull},
            {64 * MiB, 0xadf5d9dc0b09e265ull},
        };
        for (const Case &c : cases) {
            const auto order = chaseOrder(c.bytes / kLine, chaseSeed(c.bytes), {});
            QVERIFY(order.has_value());
            QCOMPARE(order->size(), c.bytes / kLine);
            const auto again = chaseOrder(c.bytes / kLine, chaseSeed(c.bytes), {});
            QCOMPARE(fnv1a(*again), fnv1a(*order));
            QCOMPARE(fnv1a(*order), c.hash);
        }
        // Different sizes, different seeds.
        QVERIFY(chaseSeed(4 * KiB) != chaseSeed(6 * KiB));
    }

    // Stop during a 2 GiB build (§14.1): the build polls often enough that
    // a cancel is seen within 250 ms in every step, and a cancel returns
    // Cancelled at once. The buffer is touched first, as the RAM test's
    // latency buffer is in prepare.
    void cancelWithin250msDuring2GibBuild()
    {
        HugeBuffer buf = touchedBuffer(2 * GiB);
        if (!buf.valid())
            QSKIP("no 2 GiB for the build on this machine");
        // A full build: the longest gap between two polls, and to the return.
        Clock::time_point last = Clock::now();
        Clock::duration worst{};
        int polls = 0;
        const auto poll = [&] {
            const auto now = Clock::now();
            worst = std::max(worst, now - last);
            last = now;
            ++polls;
            return false;
        };
        QCOMPARE(buildChaseCycle(buf.data(), 2 * GiB, kLine, chaseSeed(2 * GiB), poll), CycleBuild::Ok);
        worst = std::max(worst, Clock::now() - last);
        qInfo("2 GiB build: %d polls, longest gap %lld ms", polls,
              (long long)std::chrono::duration_cast<std::chrono::milliseconds>(worst).count());
        QVERIFY(polls >= 3 * int(2 * GiB / kLine / kCycleCancelEvery) - 1);
        QVERIFY(worst < 250ms);

        // A real cancel, in each step: the n-th poll says stop.
        for (const int at : {3, 200, 300}) {
            int seen = 0;
            Clock::time_point set{};
            const auto cancel = [&] {
                if (++seen < at)
                    return false;
                if (set == Clock::time_point{})
                    set = Clock::now();
                return true;
            };
            QCOMPARE(buildChaseCycle(buf.data(), 2 * GiB, kLine, chaseSeed(2 * GiB), cancel), CycleBuild::Cancelled);
            QVERIFY(Clock::now() - set < 250ms);
            QCOMPARE(seen, at);  // returned on the first poll that said stop
        }
    }

    // §14.1: n hops from the start = an independent reference walk.
    void hopsMatchReferenceWalk()
    {
        std::vector<std::uint64_t> mem(256 * KiB / 8);
        auto *base = reinterpret_cast<std::byte *>(mem.data());
        QCOMPARE(buildChaseCycle(base, 256 * KiB, kLine, chaseSeed(256 * KiB), {}), CycleBuild::Ok);
        for (const std::uint64_t hops : {16ull, 4096ull, 4096ull * 3 + 16, 65536ull}) {
            const void *ref = base;
            for (std::uint64_t i = 0; i < hops; ++i)
                ref = *static_cast<const void *const *>(ref);
            QCOMPARE(chaseHops(base, hops), ref);
        }
        // A whole lap ends where it started.
        QCOMPARE(chaseHops(base, 256 * KiB / kLine), static_cast<const void *>(base));
    }

    // A window's rate: hops over the time of its units, without those over
    // twice the median unit (preempted).
    void preemptedUnitsAreLeftOut()
    {
        QVERIFY(!preemptionFreeRate({}, 1000));
        QCOMPARE(*preemptionFreeRate({0.002}, 1000), 500000.0);
        // 0.009 is preempted (median 0.002); the rest are averaged.
        QCOMPARE(*preemptionFreeRate({0.009, 0.001, 0.002, 0.003}, 1000), 3000 / 0.006);
        // A skew under twice the median stays in: a mean, not a median.
        QCOMPARE(*preemptionFreeRate({0.002, 0.002, 0.0035}, 1000), 3000 / 0.0075);
        QVERIFY(!preemptionFreeRate({0.0}, 1000));
    }

    // Tiamat, checkpoint 3 (Zeb): a background thread sharing the chase core
    // made 4-12 KiB read 30% slow. Pinned to the same CPU, a competitor busy
    // a third of the time must not move the latency (preempted units left
    // out); over the whole window it would.
    void contendedCoreReadsTheSameLatency()
    {
        const std::vector<sb::isa::CoreChoice> cpus = sb::isa::allowedLogicalCpus();
        if (cpus.empty())
            QSKIP("no CPU list to pin to on this platform");
        const sb::isa::CoreChoice core = cpus.back();
        std::vector<std::uint64_t> mem(16 * KiB / 8);
        auto *base = reinterpret_cast<std::byte *>(mem.data());
        QCOMPARE(buildChaseCycle(base, 16 * KiB, kLine, chaseSeed(16 * KiB), {}), CycleBuild::Ok);
        const ChaseKernel kernel(base, 16 * KiB, kLine, hopsPerUnit(1.0));
        sb::cpu::WorkerPool::Options o;
        o.threads = 1;
        o.pinTo = core;
        sb::cpu::WorkerPool pool(o);
        const sb::cpu::WindowSpec spec{std::chrono::milliseconds(50), std::chrono::milliseconds(200), 5};
        const auto measure = [&](double *wallNs) {
            const sb::cpu::Measurement m = pool.measure(kernel, spec, [] { return false; });
            std::vector<double> wall;
            for (const sb::cpu::RepSample &r : m.reps)
                wall.push_back(r.seconds > 0 ? double(r.units) * double(kernel.hops()) / r.seconds : 0);
            *wallNs = latencyNs(sb::cpu::medianOf(wall));
            return latencyNs(m.median);
        };
        double wallIdle = 0, wallBusy = 0;
        const double idle = measure(&wallIdle);
        std::atomic<bool> stop{false};
        std::thread competitor([&] {
            sb::cpu::pinCurrentThread(core);
            while (!stop) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
                while (std::chrono::steady_clock::now() < until) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(6));
            }
        });
        const double busy = measure(&wallBusy);
        stop = true;
        competitor.join();
        qInfo("idle %.3f ns (whole window %.3f); with a competitor %.3f ns (whole window %.3f)", idle, wallIdle, busy, wallBusy);
        QVERIFY2(std::abs(idle - wallIdle) < 0.05 * wallIdle, "uncontended: the same as the whole window");
        QVERIFY2(busy < idle * 1.10, qPrintable(QStringLiteral("%1 ns against %2 ns").arg(busy).arg(idle)));
    }

    // §5.1, §9: a unit whose end pointer is outside the buffer, or inside
    // it but off a line start, is an Error.
    void rangeCheckFiresOnPlantedPointers()
    {
        std::vector<std::uint64_t> mem(64 * KiB / 8);
        auto *base = reinterpret_cast<std::byte *>(mem.data());
        const std::size_t bytes = 64 * KiB;
        // Outside: line 7 leads to a decoy that points to itself.
        {
            QCOMPARE(buildChaseCycle(base, bytes, kLine, chaseSeed(bytes), {}), CycleBuild::Ok);
            alignas(64) const void *decoy[8];
            decoy[0] = &decoy[0];
            *reinterpret_cast<const void **>(base + 7 * kLine) = &decoy[0];
            ChaseKernel k(base, bytes, kLine, kMinHopsPerUnit);
            auto t = k.makeThread(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, t->unit(0));
        }
        // Inside, off a line start: line 9 leads to base + 8, which points
        // to itself.
        {
            QCOMPARE(buildChaseCycle(base, bytes, kLine, chaseSeed(bytes), {}), CycleBuild::Ok);
            *reinterpret_cast<const void **>(base + 8) = base + 8;
            *reinterpret_cast<const void **>(base + 9 * kLine) = base + 8;
            ChaseKernel k(base, bytes, kLine, kMinHopsPerUnit);
            auto t = k.makeThread(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, t->unit(0));
        }
        // ...and an intact cycle passes, unit after unit.
        {
            QCOMPARE(buildChaseCycle(base, bytes, kLine, chaseSeed(bytes), {}), CycleBuild::Ok);
            ChaseKernel k(base, bytes, kLine, kMinHopsPerUnit);
            auto t = k.makeThread(0);
            for (int i = 0; i < 4; ++i)
                QVERIFY(t->unit(std::uint64_t(i)) < bytes);
        }
    }

    // §5.1: K = 2 ms at the estimate, a multiple of 16, in [16 Ki, 64 Mi].
    void hopsPerUnitSizing()
    {
        QCOMPARE(hopsPerUnit(1.0), std::uint64_t(2000000));
        QCOMPARE(hopsPerUnit(100.0), std::uint64_t(20000));
        QCOMPARE(hopsPerUnit(3.0), std::uint64_t(666656));  // 666,666 rounded down to 16
        QCOMPARE(hopsPerUnit(1000.0), kMinHopsPerUnit);     // 2,000: the floor
        QCOMPARE(hopsPerUnit(0.001), kMaxHopsPerUnit);      // 2 G: the ceiling
        // No usable estimate: the smallest unit, safe at any latency (AP-5 M3).
        QCOMPARE(hopsPerUnit(0.0), kMinHopsPerUnit);
        QCOMPARE(hopsPerUnit(-3.0), kMinHopsPerUnit);
        QCOMPARE(hopsPerUnit(std::numeric_limits<double>::quiet_NaN()), kMinHopsPerUnit);
        QCOMPARE(hopsPerUnit(std::numeric_limits<double>::infinity()), kMinHopsPerUnit);
        for (const double e : {0.4, 0.9, 1.7, 4.2, 12.0, 85.0, 140.0, 700.0})
            QCOMPARE(hopsPerUnit(e) % kHopsPerIteration, std::uint64_t(0));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, ChaseKernel(nullptr, 0, kLine, 17));
    }

    // P15 through the engine's P14 ceiling: exactly 3 cycles at 7 GHz.
    void plausibilityFloorIsThreeCyclesAt7GHz()
    {
        const double ceiling = sb::cpu::plausibilityCeiling(kChaseMaxHopsPerCycle, 1);
        QVERIFY(qAbs(latencyNs(ceiling) - 3.0 / 7.0) < 1e-9);
        QCOMPARE(latencyNs(2e9), 0.5);
        QCOMPARE(latencyNs(0), 0.0);
    }

    // §3, §5.1: the warm-up is one lap, clamped.
    void warmupIsOneLapClamped()
    {
        // 1 GiB / 64 B x 100 ns = 1.68 s: the targets' 1 s cap.
        QCOMPARE(chaseWarmup(GiB, kLine, 100.0, 100ms, 1000ms), 1000ms);
        // 32 KiB x 1 ns = 0.5 us: the grid's 10 ms floor.
        QCOMPARE(chaseWarmup(32 * KiB, kLine, 1.0, 10ms, 300ms), 10ms);
        // 64 MiB at 10 ns: 10.5 ms, rounded up.
        QCOMPARE(chaseWarmup(64 * MiB, kLine, 10.0, 10ms, 300ms), 11ms);
        QCOMPARE(chaseWarmup(64 * MiB, kLine, 0.0, 10ms, 300ms), 10ms);
    }

    // Through the engine: K stays the unit's work, the rate is hops per
    // second (from the median unit: close to the whole window's on an
    // uncontended core), and DRAM reads slower than L1 (a factor no loaded
    // runner closes; no absolute speed is asserted).
    void measuredDramSlowerThanL1()
    {
        const std::size_t small = 16 * KiB, large = 256 * MiB;
        HugeBuffer buf = touchedBuffer(large);
        QVERIFY2(buf.valid(), buf.error().c_str());
        sb::cpu::WorkerPool pool({1, sb::isa::CoreChoice{}});
        double ns[2] = {};
        int i = 0;
        for (const std::size_t size : {small, large}) {
            QCOMPARE(buildChaseCycle(buf.data(), size, kLine, chaseSeed(size), {}), CycleBuild::Ok);
            ChaseKernel k(buf.data(), size, kLine, hopsPerUnit(size == small ? 1.0 : 100.0));
            const sb::cpu::Measurement m = pool.measure(k, {20ms, 60ms, 3}, [] { return false; });
            QVERIFY2(m.ok(), m.error.c_str());
            QCOMPARE(k.workPerUnit(), double(k.hops()));
            for (const sb::cpu::RepSample &r : m.reps)
                QVERIFY(qAbs(r.rate * r.seconds / double(r.units) - double(k.hops())) < 0.25 * double(k.hops()));
            ns[i++] = latencyNs(m.median);
        }
        qInfo("chase: %.2f ns at 16 KiB, %.1f ns at 256 MiB", ns[0], ns[1]);
        QVERIFY(ns[0] > 0);
        QVERIFY(ns[1] > 4 * ns[0]);
    }
};

QTEST_GUILESS_MAIN(TestRamChase)
#include "tst_ram_chase.moc"
