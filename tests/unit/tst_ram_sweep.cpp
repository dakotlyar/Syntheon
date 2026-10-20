// SPDX-License-Identifier: GPL-3.0-or-later
// The RAM test's sweep (Phase 3 design §5.2, §14.1 "Targets / knees"): the
// half-octave grid; the scored target sizes for the fixture machines
// (Tiamat CPU 0 and CPU 9, Knowhere, the intel-hybrid P-core, the ARM
// big.LITTLE board), an Oryon-like L1/L2-only core, a "too close" L3 and no
// topology at all; and the knee fallback on synthetic curves with 2, 3 and
// 4 plateaus and a noisy one.
#include "isa/cache_topology.h"
#include "tests/ram/sweep/grid.h"
#include "tests/ram/sweep/knees.h"
#include "tests/ram/sweep/targets.h"
#include "tests/ram/sweep/warmup.h"

#include <QTest>

#include <cmath>
#include <string>

using namespace sb::ram;
using sb::isa::CacheLevel;
using sb::isa::CacheTopology;

namespace {

constexpr std::uint64_t KiB = 1024, MiB = 1024 * KiB, GiB = 1024 * MiB;

std::string fixture(const char *machine)
{
    return std::string(SB_FIXTURES) + "/linux/" + machine + "/sys";
}

CacheTopology topology(std::uint64_t l1, std::uint64_t l2, std::uint64_t l3)
{
    CacheTopology t;
    int level = 1;
    for (const std::uint64_t s : {l1, l2, l3}) {
        if (s) {
            CacheLevel c;
            c.level = level;
            c.type = level == 1 ? CacheLevel::Type::Data : CacheLevel::Type::Unified;
            c.sizeBytes = s;
            c.lineSize = 64;
            t.levels.push_back(c);
        }
        ++level;
    }
    return t;
}

// A curve with plateaus: up to each edge (bytes) the latency is the
// plateau's; past the last edge, `dram`. The step between plateaus spans
// two grid points (a real transition is never a single step).
std::vector<CurvePoint> curve(const std::vector<std::uint64_t> &edges, const std::vector<double> &plateaus, double noise = 0)
{
    std::vector<CurvePoint> c;
    int n = 0;
    for (const std::uint64_t b : latencyGrid(GiB)) {
        std::size_t level = 0;
        while (level < edges.size() && b > edges[level])
            ++level;
        double ns = plateaus[level];
        // The first point past an edge is halfway up (log-linear).
        if (level > 0 && level < plateaus.size() && b <= edges[level - 1] * 3 / 2)
            ns = std::sqrt(plateaus[level - 1] * plateaus[level]);
        // Deterministic +-noise.
        ns *= 1.0 + noise * ((n++ % 3) - 1);
        c.push_back({b, ns});
    }
    return c;
}

} // namespace

class TestRamSweep : public QObject
{
    Q_OBJECT

private slots:
    // Defect A: Tiamat's idle chase core sits at ~4.38 GHz for ~1.1 s with a
    // steady rate, then climbs to 5.7 GHz: steady windows alone aren't warm
    // while the clock is under 85% of the top; with no clock known, a
    // second of agreeing windows is.
    void chaseCoreWarmUp()
    {
        const std::vector<double> plateau(20, 1.09e9);  // 0.915 ns, 4.38 GHz
        QVERIFY(!chaseCoreWarm(plateau, 4380, 5756));
        QVERIFY(warmRatesSteady(plateau, 20));  // steady, and still not warm
        std::vector<double> ramped = plateau;
        ramped.insert(ramped.end(), {1.26e9, 1.41e9, 1.42e9});
        QVERIFY(!chaseCoreWarm({1.09e9, 1.26e9}, 5300, 5756));  // fast clock, rate still moving
        QVERIFY(chaseCoreWarm(ramped, 5700, 5756));
        // No clock or no top (Windows, no cpufreq): never warm early, however
        // steady (Dave, 2026-10-20: the caller spins its full cap).
        QVERIFY(!chaseCoreWarm(std::vector<double>(100, 1.42e9), std::nullopt, std::nullopt));
        QVERIFY(!chaseCoreWarm(std::vector<double>(100, 1.42e9), 5700, std::nullopt));
        QVERIFY(!chaseCoreWarm(std::vector<double>(100, 1.42e9), std::nullopt, 5756));
        QVERIFY(!chaseCoreWarm({}, 5700, 5756));
        QVERIFY(!chaseCoreWarm({1.42e9}, 5700, 5756));
        // 1% is steady, 2% isn't.
        QVERIFY(chaseCoreWarm({1.40e9, 1.41e9}, 5700, 5756));
        QVERIFY(!chaseCoreWarm({1.38e9, 1.41e9}, 5700, 5756));
    }

    void gridIsHalfOctaves()
    {
        const auto g1 = latencyGrid(GiB);
        QCOMPARE(g1.size(), std::size_t(37));
        QCOMPARE(g1.front(), std::uint64_t(4 * KiB));
        QCOMPARE(g1[1], std::uint64_t(6 * KiB));
        QCOMPARE(g1.back(), GiB);
        QVERIFY(std::is_sorted(g1.begin(), g1.end()));
        const auto g2 = latencyGrid(2 * GiB);
        QCOMPARE(g2.size(), std::size_t(39));
        QCOMPARE(g2[g2.size() - 2], 3 * GiB / 2);
        QCOMPARE(g2.back(), 2 * GiB);
        QVERIFY(latencyGrid(2 * KiB).empty());
        for (const std::uint64_t b : g2)
            QCOMPARE(b % 64, std::uint64_t(0));
    }

    // §5.2's table: Tiamat CPU 9 (the frequency CCD's top-ranked core;
    // 48 KiB / 1 MiB / 32 MiB) and CPU 0 (the 96 MiB V-cache CCD).
    void tiamatTargets()
    {
        const LatencyTargets f = targetsFromTopology(sb::isa::linuxCacheTopology(fixture("tiamat"), 9));
        QVERIFY(!f.fromCurve);
        QCOMPARE(f.level(1).bytes, 24 * KiB);
        QCOMPARE(f.level(2).bytes, 220 * KiB);
        QCOMPARE(f.level(3).bytes, 5792 * KiB);  // "5.7 MiB"
        QCOMPARE(f.level(3).cacheBytes, 32 * MiB);
        QCOMPARE(f.dramBytes, GiB);
        const LatencyTargets v = targetsFromTopology(sb::isa::linuxCacheTopology(fixture("tiamat"), 0));
        QCOMPARE(v.level(2).bytes, 220 * KiB);
        QCOMPARE(v.level(3).bytes, 10032 * KiB);
        QCOMPARE(v.dramBytes, 2 * GiB);  // 16 x 96 MiB -> 2 GiB
    }

    void knowhereIntelArmTargets()
    {
        const LatencyTargets k = targetsFromTopology(sb::isa::linuxCacheTopology(fixture("knowhere"), 0));
        QCOMPARE(k.level(1).bytes, 24 * KiB);
        QCOMPARE(k.level(2).bytes, 220 * KiB);
        QCOMPARE(k.level(3).bytes, 5792 * KiB);
        QCOMPARE(k.dramBytes, GiB);
        // intel-hybrid P-core: 48 KiB / 1.25 MiB / 12 MiB.
        const LatencyTargets p = targetsFromTopology(sb::isa::linuxCacheTopology(fixture("intel-hybrid"), 0));
        QCOMPARE(p.level(1).bytes, 24 * KiB);
        QCOMPARE(p.level(2).bytes, 244 * KiB);
        QCOMPARE(p.level(3).bytes, 3964 * KiB);
        QCOMPARE(p.dramBytes, GiB);
        // arm64-biglittle: 64 KiB / 512 KiB / 4 MiB.
        const LatencyTargets a = targetsFromTopology(sb::isa::linuxCacheTopology(fixture("arm64-biglittle"), 0));
        QCOMPARE(a.level(1).bytes, 32 * KiB);
        QCOMPARE(a.level(2).bytes, 180 * KiB);
        QCOMPARE(a.level(3).bytes, 1448 * KiB);
        QCOMPARE(a.dramBytes, GiB);
    }

    // D1: an Oryon-like core (96 KiB L1, 12 MiB L2, no L3).
    void noL3Core()
    {
        const LatencyTargets o = targetsFromTopology(topology(96 * KiB, 12 * MiB, 0));
        QCOMPARE(o.level(1).bytes, 48 * KiB);
        QCOMPARE(o.level(2).bytes, 1084 * KiB);
        QVERIFY(!o.level(3).present());
        QCOMPARE(o.level(3).absentReason, std::string("no L3 on this core"));
        QCOMPARE(o.dramBytes, GiB);  // 16 x 12 MiB = 192 MiB -> 1 GiB
    }

    // P5: an L3 too close in size to the L2 is absent, with the reason.
    void tooCloseL3IsAbsent()
    {
        const LatencyTargets t = targetsFromTopology(topology(32 * KiB, 2 * MiB, 3 * MiB));
        QVERIFY(t.level(2).present());
        QVERIFY(!t.level(3).present());
        QCOMPARE(t.level(3).cacheBytes, 3 * MiB);
        QCOMPARE(t.level(3).absentReason, std::string("L3 3 MiB is too close in size to the L2 (2 MiB)"));
        // ...and the 4 KiB rounding happens before the range check.
        const LatencyTargets r = targetsFromSizes({48 * KiB, 1 * MiB, 32 * MiB}, GiB, false);
        for (int k = 1; k <= 3; ++k)
            QCOMPARE(r.level(k).bytes % 4096, std::uint64_t(0));
    }

    void dramTargetRule()
    {
        QCOMPARE(dramTarget(0), GiB);
        QCOMPARE(dramTarget(32 * MiB), GiB);       // 512 MiB -> the 1 GiB floor
        QCOMPARE(dramTarget(64 * MiB), GiB);       // exactly 1 GiB
        QCOMPARE(dramTarget(65 * MiB), 2 * GiB);   // 1040 MiB -> 2 GiB
        QCOMPARE(dramTarget(96 * MiB), 2 * GiB);
        QCOMPARE(dramTarget(512 * MiB), 2 * GiB);  // the 2 GiB cap
        QCOMPARE(dramTarget(8 * GiB), 2 * GiB);
    }

    void noTopologyEveryLevelAbsent()
    {
        const LatencyTargets t = targetsFromTopology(CacheTopology{});
        for (int k = 1; k <= 3; ++k) {
            QVERIFY(!t.level(k).present());
            QCOMPARE(t.level(k).absentReason, std::string("the OS reports no caches"));
        }
        QCOMPARE(t.dramBytes, GiB);
    }

    // Four plateaus (L1 32 KiB, L2 512 KiB, L3 8 MiB, DRAM): three knees.
    void kneesFourPlateaus()
    {
        const auto c = curve({32 * KiB, 512 * KiB, 8 * MiB}, {1.0, 3.5, 12.0, 90.0});
        QCOMPARE(detectKnees(c), (std::vector<std::uint64_t>{32 * KiB, 512 * KiB, 8 * MiB}));
        const LatencyTargets t = targetsFromCurve(c, GiB);
        QVERIFY(t.fromCurve);
        QCOMPARE(t.level(1).bytes, 16 * KiB);
        QCOMPARE(t.level(2).bytes, 128 * KiB);  // sqrt(32 KiB x 512 KiB)
        QCOMPARE(t.level(3).bytes, 2 * MiB);    // sqrt(512 KiB x 8 MiB)
        QCOMPARE(t.dramBytes, GiB);
    }

    // Three plateaus (no L3): two knees; L3 absent, found nowhere.
    void kneesThreePlateaus()
    {
        const auto c = curve({64 * KiB, 4 * MiB}, {1.2, 6.0, 110.0});
        QCOMPARE(detectKnees(c), (std::vector<std::uint64_t>{64 * KiB, 4 * MiB}));
        const LatencyTargets t = targetsFromCurve(c, GiB);
        QVERIFY(t.level(1).present() && t.level(2).present());
        QVERIFY(!t.level(3).present());
        QCOMPARE(t.level(3).absentReason, std::string("no L3 found on the curve"));
    }

    // Two plateaus: one knee, only an L1; the latency group can't be scored.
    void kneesTwoPlateaus()
    {
        const auto c = curve({256 * KiB}, {2.0, 80.0});
        QCOMPARE(detectKnees(c), (std::vector<std::uint64_t>{256 * KiB}));
        const LatencyTargets t = targetsFromCurve(c, GiB);
        QVERIFY(t.level(1).present());
        QVERIFY(!t.level(2).present());
        QVERIFY(!t.level(3).present());
        // A flat curve: no knee at all.
        QVERIFY(detectKnees(curve({}, {5.0})).empty());
    }

    // +-4% noise and a one-point spike (a rise that falls straight back)
    // find the same knees.
    void kneesNoisyCurve()
    {
        auto c = curve({32 * KiB, 512 * KiB, 8 * MiB}, {1.0, 3.5, 12.0, 90.0}, 0.04);
        // A spike at 2 MiB (on the L3 plateau): +60%, then back.
        for (CurvePoint &p : c) {
            if (p.bytes == 2 * MiB)
                p.ns *= 1.6;
        }
        QCOMPARE(detectKnees(c), (std::vector<std::uint64_t>{32 * KiB, 512 * KiB, 8 * MiB}));
    }
};

QTEST_GUILESS_MAIN(TestRamSweep)
#include "tst_ram_sweep.moc"
