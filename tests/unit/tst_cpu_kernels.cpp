// SPDX-License-Identifier: GPL-3.0-or-later
// The CPU test's kernels (Phase 2 design §5, §6, §14.1 "Kernels"): every
// kernel x every seed against its golden table (the same committed tables on
// every platform), the prime counts against an independent Miller-Rabin
// count, each kernel run through the engine, and the 100 ms unit rule (§4.1).
#include "tests/cpu/engine/worker_pool.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/fp_kernel.h"
#include "tests/cpu/kernels/int_kernel.h"
#include "tests/cpu/kernels/primes_kernel.h"

#include <QTest>

#include <algorithm>
#include <chrono>

using namespace sb::cpu;
using namespace std::chrono_literals;

namespace {

// --- an independent prime test: deterministic Miller-Rabin ------------------
// Bases 2, 3, 5, 7, 11, 13 are exact for n < 3,474,749,660,383; the segments
// end below 1.0001e12.
std::uint64_t mulMod(std::uint64_t a, std::uint64_t b, std::uint64_t n)
{
    // n < 2^40, so a, b < 2^40: split b into 20-bit halves to stay in 64 bits.
    const std::uint64_t hi = (a * (b >> 20)) % n;
    return ((hi << 20) + a * (b & 0xFFFFF)) % n;
}

std::uint64_t powMod(std::uint64_t a, std::uint64_t e, std::uint64_t n)
{
    std::uint64_t r = 1;
    a %= n;
    while (e) {
        if (e & 1)
            r = mulMod(r, a, n);
        a = mulMod(a, a, n);
        e >>= 1;
    }
    return r;
}

bool isPrime(std::uint64_t n)
{
    for (const std::uint64_t p : {2ull, 3ull, 5ull, 7ull, 11ull, 13ull, 17ull, 19ull, 23ull, 29ull, 31ull, 37ull}) {
        if (n % p == 0)
            return n == p;
    }
    std::uint64_t d = n - 1;
    int s = 0;
    while ((d & 1) == 0) {
        d >>= 1;
        ++s;
    }
    for (const std::uint64_t a : {2ull, 3ull, 5ull, 7ull, 11ull, 13ull}) {
        std::uint64_t x = powMod(a, d, n);
        if (x == 1 || x == n - 1)
            continue;
        bool composite = true;
        for (int i = 1; i < s && composite; ++i) {
            x = mulMod(x, x, n);
            composite = x != n - 1;
        }
        if (composite)
            return false;
    }
    return true;
}

// The fastest of `tries` runs of fn() (the least disturbed one).
template <typename Fn>
Clock::duration fastest(int tries, Fn &&fn)
{
    Clock::duration best = Clock::duration::max();
    for (int i = 0; i < tries; ++i) {
        const auto t0 = Clock::now();
        fn();
        best = std::min(best, Clock::now() - t0);
    }
    return best;
}

double msOf(Clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

} // namespace

class TestCpuKernels : public QObject
{
    Q_OBJECT

private slots:
    void intMatchesGolden()
    {
        for (std::uint64_t s = 0; s < kSeeds; ++s)
            QCOMPARE(intUnit(s), golden::kInt[s]);
        // Different seeds give different checksums (the input really depends on it).
        QVERIFY(golden::kInt[0] != golden::kInt[1]);
        IntKernel k;
        QCOMPARE(k.golden(17), std::optional(golden::kInt[17]));
        QCOMPARE(k.makeThread(0)->unit(17 + kSeeds), golden::kInt[17]);  // unit index -> seed
    }

    void fpMatchesGolden()
    {
        for (std::uint64_t s = 0; s < kSeeds; ++s)
            QCOMPARE(fpUnit(s), golden::kFp[s]);
        QVERIFY(golden::kFp[0] != golden::kFp[1]);
        FpKernel k;
        QCOMPARE(k.makeThread(0)->unit(5 + 3 * kSeeds), golden::kFp[5]);
    }

    void primesMatchGolden()
    {
        const std::vector<std::uint32_t> base = primesBase();
        // 3 .. 1,000,033: pi(1,000,033) = 78,500 (78,498 below 10^6, then
        // 1,000,003 and 1,000,033); 2 isn't in the list.
        QCOMPARE(int(base.size()), 78499);
        QCOMPARE(base.front(), 3u);
        QCOMPARE(base.back(), 1000033u);
        std::vector<std::uint64_t> bitset(kPrimesBitsetWords);
        for (std::uint64_t s = 0; s < kSeeds; ++s)
            QCOMPARE(primesUnit(s, base, bitset.data()), golden::kPrimes[s]);
        PrimesKernel k;
        QCOMPARE(k.makeThread(0)->unit(63), golden::kPrimes[63]);
    }

    // §6.4: the sieve's counts against an independent count (Miller-Rabin of
    // every odd number) on 4 of the 64 segments, the first and last included.
    void primesAgreeWithMillerRabin_data()
    {
        QTest::addColumn<int>("seed");
        for (const int s : {0, 17, 42, 63})
            QTest::addRow("segment %d", s) << s;
    }
    void primesAgreeWithMillerRabin()
    {
        QFETCH(int, seed);
        const std::uint64_t lo = primesSegmentStart(std::uint64_t(seed));
        std::uint64_t count = 0;
        for (std::uint64_t n = lo + 1; n < lo + kPrimesSegment; n += 2)
            count += isPrime(n) ? 1 : 0;
        QCOMPARE(count, golden::kPrimes[std::size_t(seed)]);
    }

    void millerRabinSelfCheck()
    {
        QVERIFY(isPrime(1000003));
        QVERIFY(isPrime(1000033));
        QVERIFY(!isPrime(1000003ull * 1000033ull));
        QVERIFY(isPrime(999999999989ull));    // the largest prime below 10^12
        QVERIFY(isPrime(1000000000039ull));   // the smallest above
        QVERIFY(!isPrime(1000000000001ull));  // 73 x 137 x 99990001
    }

    // Each kernel through the engine: every unit checked against the golden
    // table (two threads, all seeds), a plausible rate.
    void kernelsThroughTheEngine()
    {
        const IntKernel i;
        const FpKernel f;
        const PrimesKernel p;
        WorkerPool pool({2, {}});
        for (const Kernel *k : std::initializer_list<const Kernel *>{&i, &f, &p}) {
            const Measurement m = pool.measure(*k, {20ms, 50ms, 2}, [] { return false; });
            QVERIFY2(m.ok(), m.error.c_str());
            QVERIFY(m.median > 0);
            qInfo("%s: %.1f M/s on 2 threads", k->name().c_str(), m.median / 1e6);
        }
    }

    // §4.1: a unit takes at most 100 ms on the slowest supported CPU, so Stop
    // returns within 250 ms. Here: the fastest of 3 runs, well below that
    // even on a slow runner (they take about 0.1-2 ms on Knowhere).
    void unitsAreShort()
    {
        const std::vector<std::uint32_t> base = primesBase();
        std::vector<std::uint64_t> bitset(kPrimesBitsetWords);
        const Clock::duration ti = fastest(3, [] { intUnit(3); });
        const Clock::duration tf = fastest(3, [] { fpUnit(3); });
        const Clock::duration tp = fastest(3, [&] { primesUnit(3, base, bitset.data()); });
        qInfo("unit times: integer %.3f ms, FP %.3f ms, primes %.3f ms", msOf(ti), msOf(tf), msOf(tp));
        for (const Clock::duration t : {ti, tf, tp})
            QVERIFY(t < 25ms);
    }
};

QTEST_GUILESS_MAIN(TestCpuKernels)
#include "tst_cpu_kernels.moc"
