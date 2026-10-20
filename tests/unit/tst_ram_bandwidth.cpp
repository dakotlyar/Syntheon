// SPDX-License-Identifier: GPL-3.0-or-later
// The RAM test's bandwidth kernels (Phase 3 design §5.3, §9, §14.1
// "Bandwidth"): on every path this runner has, the read sum is exact and
// write / copy store exactly the pattern, with both store variants on x64;
// the sampled checks catch a planted skipped block and a corrupted word;
// the variants (the x64 *.plain pair, absent on ARM64); Stop within 250 ms
// in the middle of an all-thread window; the P9 buffer plan and the
// machine's total last-level cache; the P15 ceiling.
#include "isa/cache_topology.h"
#include "isa/cpu_features.h"
#include "tests/cpu/engine/platform_threads.h"
#include "tests/cpu/engine/worker_pool.h"
#include "tests/ram/kernels/bw_kernels.h"
#include "tests/ram/memory/huge_buffer.h"

#include <QTest>

#include <atomic>
#include <chrono>
#include <thread>

using namespace sb::ram;
using namespace std::chrono_literals;
using sb::cpu::Clock;

namespace {

constexpr std::size_t KiB = 1024, MiB = 1024 * KiB;
constexpr std::uint64_t GiB = 1024 * MiB;

#if defined(__x86_64__) || defined(_M_X64)
constexpr bool kX64 = true;
#else
constexpr bool kX64 = false;
#endif

// The paths this runner can execute.
std::vector<BwPath> runnablePaths()
{
    if (!kX64)
        return {BwPath::Neon};
    std::vector<BwPath> paths{BwPath::Sse2};
    if (sb::isa::detect().has(sb::isa::Feature::Avx2))
        paths.push_back(BwPath::Avx2);
    return paths;
}

const char *pathName(BwPath p)
{
    return p == BwPath::Avx2 ? "AVX2" : p == BwPath::Sse2 ? "SSE2" : "NEON";
}

// A 64-byte aligned scratch region (heap; tests only).
struct Region {
    explicit Region(std::size_t bytes) : words(bytes / 8 + 8)
    {
        auto at = reinterpret_cast<std::uintptr_t>(words.data());
        data = reinterpret_cast<std::byte *>((at + 63) / 64 * 64);
    }
    std::vector<std::uint64_t> words;
    std::byte *data = nullptr;
    std::uint64_t *w() const { return reinterpret_cast<std::uint64_t *>(data); }
};

std::string fixture(const char *machine)
{
    return std::string(SB_FIXTURES) + "/linux/" + machine + "/sys";
}

} // namespace

class TestRamBandwidth : public QObject
{
    Q_OBJECT

private slots:
    // Each entry point, called directly, on a region of 1 MiB + 512 B (one
    // whole chunk past the cell): the sum and the stores are exact, word for
    // word.
    void entryPointsAreExact()
    {
        const std::size_t bytes = MiB + 512;
        for (const BwPath path : runnablePaths()) {
            const BwFunctions f = bandwidthFunctions(path);
            QVERIFY(f.read && f.write && f.copy);
            QCOMPARE(f.writeNt != nullptr, kX64);
            QCOMPARE(f.copyNt != nullptr, kX64);
            Region a(bytes), b(bytes);
            std::uint64_t want = 0;
            for (std::size_t i = 0; i < bytes / 8; ++i) {
                a.w()[i] = 0x0123456789ABCDEFull * (i + 1);
                want += a.w()[i];
            }
            QCOMPARE(f.read(a.data, bytes), want);
            for (auto *write : {f.write, f.writeNt}) {
                if (!write)
                    continue;
                write(b.data, bytes, 0xFFFFFFFFFFFFFF00ull);  // wraps past 2^64: still base + i
                for (std::size_t i = 0; i < bytes / 8; ++i) {
                    if (b.w()[i] != 0xFFFFFFFFFFFFFF00ull + i)
                        QFAIL(qPrintable(QStringLiteral("%1 write: word %2").arg(pathName(path)).arg(i)));
                }
            }
            for (auto *copy : {f.copy, f.copyNt}) {
                if (!copy)
                    continue;
                std::fill(b.words.begin(), b.words.end(), 0);
                copy(b.data, a.data, bytes);
                QVERIFY2(std::equal(a.w(), a.w() + bytes / 8, b.w()), pathName(path));
            }
        }
    }

    // §5.3, D4: x64 has the plain pair (shown, not scored); ARM64 doesn't.
    void variantsPerArchitecture()
    {
        const auto x = bandwidthVariants(BwPath::Avx2);
        QCOMPARE(x.size(), std::size_t(5));
        QCOMPARE(x[0].key, std::string("read"));
        QCOMPARE(x[1].key, std::string("write"));
        QVERIFY(x[1].stores == BwStores::NonTemporal && x[1].scored);
        QCOMPARE(x[2].key, std::string("copy"));
        QVERIFY(x[2].stores == BwStores::NonTemporal && x[2].scored);
        QCOMPARE(x[3].key, std::string("write.plain"));
        QVERIFY(x[3].stores == BwStores::Plain && !x[3].scored);
        QCOMPARE(x[4].key, std::string("copy.plain"));
        QVERIFY(x[4].stores == BwStores::Plain && !x[4].scored);
        QCOMPARE(bandwidthVariants(BwPath::Sse2).size(), std::size_t(5));
        const auto a = bandwidthVariants(BwPath::Neon);
        QCOMPARE(a.size(), std::size_t(3));
        for (const BwVariant &v : a) {
            QVERIFY(v.scored);
            QVERIFY(v.stores == BwStores::Plain);
            QVERIFY(v.key.find(".plain") == std::string::npos);
        }
        QCOMPARE(bandwidthPathLabel(BwPath::Neon), std::string("NEON loads and plain stores"));
        QCOMPARE(bandwidthPathLabel(BwPath::Avx2),
                 std::string("AVX2 loads; write/copy scored with non-temporal stores, plain stores also shown"));
    }

    void pathChoice()
    {
        sb::isa::CpuFeatures f;
        f.set(sb::isa::Feature::Sse2);
        QCOMPARE(chooseBandwidthPath(f), kX64 ? BwPath::Sse2 : BwPath::Neon);
        f.set(sb::isa::Feature::Avx2);
        QCOMPARE(chooseBandwidthPath(f), kX64 ? BwPath::Avx2 : BwPath::Neon);
        f.set(sb::isa::Feature::Avx512F);  // no AVX-512 path (P8)
        QCOMPARE(chooseBandwidthPath(f), kX64 ? BwPath::Avx2 : BwPath::Neon);
    }

    // The pattern table follows the kernels through fill, write and copy
    // units: every read unit's sum stays exact, every write and copy unit
    // passes its sampled check, for every variant, single- and two-thread.
    void unitsKeepThePatternExact()
    {
        for (const BwPath path : runnablePaths()) {
            Region mem(2 * 32 * MiB);
            BandwidthSlice s0(mem.data, 32 * MiB), s1(mem.data + 32 * MiB, 32 * MiB);
            QVERIFY(s0.fill(1, {}));
            QVERIFY(s1.fill(2, {}));
            for (const BwVariant &v : bandwidthVariants(path)) {
                BandwidthKernel st(path, v, {&s0, &s1}, 1);
                QCOMPARE(st.blockBytes(), kBwBlock);
                QCOMPARE(st.workPerUnit(), double(kBwBlock));
                auto t = st.makeThread(0);
                for (std::uint64_t u = 0; u < 6; ++u)  // 2 slices x 2 blocks, and round again
                    t->unit(u);
                BandwidthKernel mt(path, v, {&s0, &s1}, 2);
                auto t1 = mt.makeThread(1);
                t1->unit(9);
            }
            // After all that, a read of everything is still exact.
            BandwidthKernel read(path, bandwidthVariants(path)[0], {&s0, &s1}, 1);
            auto r = read.makeThread(0);
            for (std::uint64_t u = 0; u < 4; ++u)
                r->unit(u);
        }
    }

    // §9: a corrupted word makes a read unit an Error.
    void readCatchesACorruptedWord()
    {
        const BwPath path = runnablePaths().back();
        Region mem(16 * MiB);
        BandwidthSlice s(mem.data, 16 * MiB);
        QVERIFY(s.fill(7, {}));
        mem.w()[3 * MiB / 8 + 12345] ^= 1;
        BandwidthKernel k(path, bandwidthVariants(path)[0], {&s}, 1);
        auto t = k.makeThread(0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, t->unit(0));
    }

    // §14.1: the sampled check catches a planted skipped block, whichever
    // line of the cell it samples: a cell left as it was before a write,
    // and a cell of a copy's destination that wasn't copied.
    void sampledCheckCatchesSkippedBlock()
    {
        const BwPath path = runnablePaths().back();
        const BwFunctions f = bandwidthFunctions(path);
        Region mem(8 * MiB);
        BandwidthSlice s(mem.data, 8 * MiB);
        QVERIFY(s.fill(3, {}));
        std::vector<std::uint64_t> before(mem.w() + 2 * MiB / 8, mem.w() + 3 * MiB / 8);
        f.write(mem.data, 4 * MiB, 1000);
        // "Skip" the third MiB: put back what was there.
        std::copy(before.begin(), before.end(), mem.w() + 2 * MiB / 8);
        s.wrote(0, 4 * MiB, 1000);
        for (std::uint64_t salt = 0; salt < 32; ++salt)
            QVERIFY(!s.verifySample(0, 4 * MiB, salt));
        // ...and an intact write passes.
        f.write(mem.data, 4 * MiB, 2000);
        s.wrote(0, 4 * MiB, 2000);
        for (std::uint64_t salt = 0; salt < 32; ++salt)
            QVERIFY(s.verifySample(0, 4 * MiB, salt));
        // A copy of the first half's 4 cells that skips the second one.
        f.copy(mem.data + 4 * MiB, mem.data, 4 * MiB);
        f.write(mem.data + 5 * MiB, MiB, 77);  // the destination's 2nd cell, overwritten
        for (std::uint64_t salt = 0; salt < 32; ++salt)
            QVERIFY(!s.verifyCopySample(4 * MiB, 0, 4 * MiB, salt));
        f.copy(mem.data + 4 * MiB, mem.data, 4 * MiB);
        QVERIFY(s.verifyCopySample(4 * MiB, 0, 4 * MiB, 5));
    }

    // Through the engine, single-thread and all-thread: every variant of
    // every runnable path measures, with checks on, and the rate is the
    // units' bytes (no absolute speed is asserted).
    void measuresThroughTheEngine()
    {
        const int threads = std::min(4, sb::cpu::logicalCpuCount());
        std::vector<HugeBuffer> buffers;
        std::vector<BandwidthSlice> slices;
        slices.reserve(std::size_t(threads));
        for (int i = 0; i < threads; ++i) {
            buffers.push_back(HugeBuffer::allocate(32 * MiB));
            QVERIFY2(buffers.back().valid(), buffers.back().error().c_str());
            slices.emplace_back(buffers.back().data(), 32 * MiB);
            QVERIFY(slices.back().fill(std::uint64_t(i), {}));
        }
        std::vector<BandwidthSlice *> all;
        for (BandwidthSlice &s : slices)
            all.push_back(&s);
        sb::cpu::WorkerPool st({1, sb::isa::CoreChoice{}});
        sb::cpu::WorkerPool mt({threads, sb::isa::CoreChoice{}});
        for (const BwPath path : runnablePaths()) {
            for (const BwVariant &v : bandwidthVariants(path)) {
                for (const bool single : {true, false}) {
                    BandwidthKernel k(path, v, all, single ? 1 : threads);
                    const sb::cpu::Measurement m =
                        (single ? st : mt).measure(k, {20ms, 50ms, 3}, [] { return false; });
                    QVERIFY2(m.ok(), (k.name() + ": " + m.error).c_str());
                    QVERIFY(m.median > 0);
                    qInfo("%s %s %s: %.1f GB/s", pathName(path), single ? "1T" : "MT", k.name().c_str(),
                          m.median / 1e9);
                }
            }
        }
    }

    // §14.1: Stop within 250 ms in the middle of an all-thread window.
    void stopWithin250msMidWindow()
    {
        const int threads = std::min(4, sb::cpu::logicalCpuCount());
        std::vector<HugeBuffer> buffers;
        std::vector<BandwidthSlice> slices;
        slices.reserve(std::size_t(threads));
        std::vector<BandwidthSlice *> all;
        for (int i = 0; i < threads; ++i) {
            buffers.push_back(HugeBuffer::allocate(32 * MiB));
            QVERIFY(buffers.back().valid());
            slices.emplace_back(buffers.back().data(), 32 * MiB);
            QVERIFY(slices.back().fill(std::uint64_t(i), {}));
        }
        for (BandwidthSlice &s : slices)
            all.push_back(&s);
        const BwPath path = runnablePaths().back();
        sb::cpu::WorkerPool pool({threads, sb::isa::CoreChoice{}});
        for (const BwVariant &v : bandwidthVariants(path)) {
            BandwidthKernel k(path, v, all, threads);
            std::atomic<bool> cancel{false};
            Clock::time_point setAt{};
            std::thread stopper([&] {
                std::this_thread::sleep_for(300ms);  // past the warm-up, into the first repetition
                setAt = Clock::now();
                cancel = true;
            });
            const sb::cpu::Measurement m = pool.measure(k, {100ms, 10s, 5}, [&] { return cancel.load(); });
            const auto returned = Clock::now();
            stopper.join();
            QCOMPARE(m.outcome, sb::cpu::Measurement::Outcome::Cancelled);
            QVERIFY2(returned - setAt < 250ms, k.name().c_str());
        }
    }

    // P9 on Dave's machines and at the edges.
    void bufferPlan()
    {
        // Tiamat: 32 + 96 MiB of L3, 32 threads -> 1 GiB as 32 x 32 MiB.
        BandwidthPlan t = planBandwidth(128 * MiB, 32);
        QCOMPARE(t.sliceBytes, 32 * MiB);
        QCOMPARE(t.blockBytes, kBwBlock);
        QCOMPARE(t.totalBytes(), GiB);
        QCOMPARE(t.describe(), std::string("1 GiB in total (32 MiB per thread)"));
        // Knowhere: 2 x 32 MiB, 24 threads -> 42.7 MiB, rounded up to 48.
        BandwidthPlan k = planBandwidth(64 * MiB, 24);
        QCOMPARE(k.sliceBytes, 48 * MiB);
        QCOMPARE(k.totalBytes(), 1152 * MiB);
        QCOMPARE(k.describe(), std::string("1152 MiB in total (48 MiB per thread)"));
        // A slice below the block: the block is the slice.
        BandwidthPlan many = planBandwidth(32 * MiB, 256);
        QCOMPARE(many.sliceBytes, 4 * MiB);
        QCOMPARE(many.blockBytes, 4 * MiB);
        // A huge LLC: 8 x 1 GiB -> the 4 GiB cap; rounding up would pass it,
        // so it rounds down to 16 MiB multiples.
        BandwidthPlan big = planBandwidth(GiB, 24);
        QCOMPARE(big.sliceBytes, 160 * MiB);
        QVERIFY(big.totalBytes() <= 4 * GiB);
        // One thread: the whole total.
        QCOMPARE(planBandwidth(0, 1).sliceBytes, std::size_t(GiB));
        for (const int n : {1, 2, 3, 6, 8, 12, 16, 20, 24, 28, 32, 64, 96, 128}) {
            for (const std::uint64_t llc : {std::uint64_t(0), 8 * GiB / 1024, 128 * GiB / 1024, 384 * GiB / 1024, 2 * GiB}) {
                const BandwidthPlan p = planBandwidth(llc, n);
                QCOMPARE(p.sliceBytes % kBwSliceUnit, std::size_t(0));
                QCOMPARE(p.sliceBytes % p.blockBytes, std::size_t(0));
                QVERIFY(p.totalBytes() >= std::min<std::uint64_t>(4 * GiB, std::max<std::uint64_t>(GiB, 8 * llc))
                        || p.totalBytes() + std::uint64_t(n) * kBwBlock > 4 * GiB);
                QVERIFY(p.totalBytes() <= 4 * GiB + std::uint64_t(n) * kBwSliceUnit);
            }
        }
    }

    // Each distinct last-level cache counted once (Tiamat's two CCDs).
    void totalLastLevelCacheOfTiamat()
    {
        std::vector<sb::isa::CacheTopology> cpus;
        for (int c = 0; c < 32; ++c)
            cpus.push_back(sb::isa::linuxCacheTopology(fixture("tiamat"), c));
        QCOMPARE(totalLastLevelCache(cpus), std::uint64_t(128 * MiB));
        QCOMPARE(totalLastLevelCache({}), std::uint64_t(0));
    }

    void plausibilityCeilingIs128BytesPerCycle()
    {
        QCOMPARE(sb::cpu::plausibilityCeiling(kBwMaxBytesPerCycle, 1), 128.0 * 7e9);
        QCOMPARE(sb::cpu::plausibilityCeiling(kBwMaxBytesPerCycle, 32), 32 * 128.0 * 7e9);
    }

    void rejectsBadConfigurations()
    {
        Region mem(8 * MiB);
        BandwidthSlice a(mem.data, 4 * MiB), b(mem.data + 4 * MiB, 4 * MiB);
        const BwPath path = runnablePaths().back();
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, BandwidthKernel(path, bandwidthVariants(path)[0], {&a, &b}, 3));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, BandwidthSlice(mem.data, 3 * MiB));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, BandwidthSlice(mem.data + 8, 2 * MiB));
        // The other architecture's path has no code in this build.
        const BwPath other = kX64 ? BwPath::Neon : BwPath::Avx2;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, BandwidthKernel(other, bandwidthVariants(other)[0], {&a}, 1));
        if (!kX64) {
            const BwVariant nt{BwOp::Write, BwStores::NonTemporal, true, "write"};
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, BandwidthKernel(BwPath::Neon, nt, {&a}, 1));
        }
    }

    // A cancelled fill stops at a 64 MiB boundary.
    void fillIsCancellable()
    {
        Region mem(256 * MiB);
        BandwidthSlice s(mem.data, 256 * MiB);
        int polls = 0;
        QVERIFY(!s.fill(1, [&] { return ++polls == 3; }));
        QCOMPARE(polls, 3);
        // What was written is still vouched for; the rest is unchecked.
        QVERIFY(s.verifySample(0, 128 * MiB, 9));
    }
};

QTEST_GUILESS_MAIN(TestRamBandwidth)
#include "tst_ram_bandwidth.moc"
