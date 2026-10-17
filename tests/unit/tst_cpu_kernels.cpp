// SPDX-License-Identifier: GPL-3.0-or-later
// The CPU test's kernels (Phase 2 design §5, §6, §14.1 "Kernels"): every
// kernel x every seed against its golden table (the same committed tables on
// every platform), the prime counts against an independent Miller-Rabin
// count, each kernel run through the engine, and the 100 ms unit rule (§4.1).
#include "tests/cpu/engine/platform_threads.h"
#include "tests/cpu/engine/worker_pool.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/fp_kernel.h"
#include "tests/cpu/kernels/int_kernel.h"
#include "tests/cpu/kernels/nbody_kernel.h"
#include "tests/cpu/kernels/primes_kernel.h"
#include "tests/cpu/kernels/prng.h"
#include "tests/cpu/kernels/simd_kernel.h"
#include "tests/cpu/kernels/sort_kernel.h"

#include <QTest>

#if defined(Q_OS_WIN)
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

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

// The SIMD paths this runner's CPU (and OS) can run: the widest one and every
// narrower one on the same ladder (P9).
std::vector<sb::isa::SimdPath> runnableSimdPaths()
{
    using sb::isa::SimdPath;
    std::vector<SimdPath> out;
    switch (sb::isa::bestSimdPath(sb::isa::detect())) {
    case SimdPath::Avx512: out.push_back(SimdPath::Avx512); [[fallthrough]];
    case SimdPath::Avx2Fma: out.push_back(SimdPath::Avx2Fma); [[fallthrough]];
    case SimdPath::Sse42: out.push_back(SimdPath::Sse42); break;
    case SimdPath::Sve: out.push_back(SimdPath::Sve); [[fallthrough]];
    case SimdPath::Neon: out.push_back(SimdPath::Neon); break;
    case SimdPath::None: break;
    }
    return out;
}

// This thread's CPU time (user + kernel), in seconds.
double threadCpuSeconds()
{
#if defined(Q_OS_WIN)
    FILETIME created, exited, kernel, user;
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    const auto ticks = [](const FILETIME &f) { return (std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) * 1e-7;
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
#endif
}

// The CPU time one call of fn() takes on this thread: the average over calls
// adding up to at least 0.2 s of CPU (Windows counts thread time in 15.6 ms
// ticks), at least 3 calls. CPU time, not wall time, so a loaded runner that
// preempts the thread doesn't stretch it (AP-5 L7).
template <typename Fn>
Clock::duration cpuPerCall(Fn &&fn)
{
    const double start = threadCpuSeconds();
    int calls = 0;
    double used = 0;
    while (calls < 3 || used < 0.2) {
        fn();
        ++calls;
        used = threadCpuSeconds() - start;
    }
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(used / calls));
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

    // §5.3: the inputs keep the arithmetic exact (A, B in -2..2; x, y in
    // -128..127), and the reference matches the golden table.
    void simdReferenceMatchesGolden()
    {
        const SimdInputs inputs;
        for (const std::uint64_t s : {0ull, 1ull, 63ull}) {
            const simd::Input in = inputs.forSeed(s);
            for (int i = 0; i < simd::kGemmM * simd::kGemmK; ++i)
                QVERIFY(in.a[i] >= -2 && in.a[i] <= 2 && in.a[i] == float(int(in.a[i])));
            for (int i = 0; i < simd::kGemmBRows * simd::kGemmN; ++i)
                QVERIFY(in.b[i] >= -2 && in.b[i] <= 2 && in.b[i] == float(int(in.b[i])));
            for (int i = 0; i < simd::kDotLength; ++i)
                QVERIFY(in.x[i] >= -128 && in.x[i] <= 127);
            simd::Output out{};
            simdReferenceUnit(in, out);
            QCOMPARE(simdChecksum(out), golden::kSimd[s]);
            // The largest |C| stays far inside the exact range of a float.
            float most = 0;
            for (const float c : out.c)
                most = std::max(most, std::abs(c));
            QVERIFY(most < float(1 << 24));
            QVERIFY(most > 0);
        }
        QVERIFY(golden::kSimd[0] != golden::kSimd[1]);
    }

    // §14.1: every path this runner has gives the reference's result, on
    // every seed. The others are skipped (and named).
    void simdPathsMatchGolden()
    {
        const SimdInputs inputs;
        const std::vector<sb::isa::SimdPath> paths = runnableSimdPaths();
        QVERIFY(!paths.empty());
        for (const sb::isa::SimdPath p :
             {sb::isa::SimdPath::Sse42, sb::isa::SimdPath::Avx2Fma, sb::isa::SimdPath::Avx512, sb::isa::SimdPath::Neon,
              sb::isa::SimdPath::Sve}) {
            const bool built = simdUnitFor(p) != nullptr;
            if (std::find(paths.begin(), paths.end(), p) == paths.end() || !built) {
                qInfo("%s: not run here (%s)", sb::isa::simdPathName(p),
                      built ? "this CPU or OS lacks it" : "not in this build");
                continue;
            }
            std::uint32_t bits = 0;
            for (std::uint64_t s = 0; s < kSeeds; ++s) {
                const simd::Input in = inputs.forSeed(s);
                simd::Output out{};
                simdUnitFor(p)(&in, &out);
                QVERIFY2(simdChecksum(out) == golden::kSimd[s],
                         qPrintable(QStringLiteral("%1, seed %2").arg(QLatin1StringView(sb::isa::simdPathName(p))).arg(s)));
                bits = out.vectorBits;
            }
            qInfo("%s: all %d seeds match the reference (%u-bit)", sb::isa::simdPathName(p), int(kSeeds), bits);
            // And through the engine, as the CPU test runs it.
            const SimdKernel k(inputs, p, bits);
            WorkerPool pool({2, {}});
            const Measurement m = pool.measure(k, {20ms, 50ms, 2}, [] { return false; });
            QVERIFY2(m.ok(), m.error.c_str());
            qInfo("%s: %.0f M ops/s on 2 threads", sb::isa::simdPathName(p), m.median / 1e6);
        }
    }

    // P9 / D15: the path the CPU test picks on this machine is the widest one,
    // with its label and the reason for the log.
    void simdChoiceOnThisMachine()
    {
        const SimdInputs inputs;
        const sb::isa::CpuFeatures f = sb::isa::detect();
        const SimdChoice c = chooseSimdPath(f, inputs);
        QCOMPARE(c.path, sb::isa::bestSimdPath(f));
        QVERIFY(simdUnitFor(c.path) != nullptr);
        QVERIFY(!c.warning);
        QCOMPARE(QString::fromStdString(c.label), QString::fromLatin1(sb::isa::simdPathName(c.path)));
        qInfo("SIMD choice: %s; %s", c.label.c_str(), c.note.c_str());
        QCOMPARE(simdPathLabel(sb::isa::SimdPath::Sve, 256), std::string("SVE (256-bit, unvalidated)"));
        QCOMPARE(simdPathLabel(sb::isa::SimdPath::Avx2Fma, 256), std::string("AVX2 + FMA (256-bit)"));
        // A path not in this build can't be made into a running kernel.
        const sb::isa::SimdPath foreign =
            simdUnitFor(sb::isa::SimdPath::Neon) ? sb::isa::SimdPath::Avx512 : sb::isa::SimdPath::Neon;
        const SimdKernel k(inputs, foreign, 128);
        WorkerPool pool({1, {}});
        QCOMPARE(pool.measure(k, {10ms, 10ms, 1}, [] { return false; }).outcome, Measurement::Outcome::Failed);
    }

    // §5.6: our introsort against std::sort, on the shapes that break naive
    // quicksorts (sorted, reversed, all equal, organ pipe, few values) and on
    // every size up to 300 (the insertion-sort threshold and the recursion
    // around it).
    void introsortAgreesWithStdSort()
    {
        Xoshiro256ss rng(99);
        std::vector<std::vector<std::uint64_t>> cases;
        for (std::size_t n = 0; n <= 300; ++n) {
            std::vector<std::uint64_t> v(n);
            for (std::uint64_t &x : v)
                x = rng.below(n / 3 + 1);  // many duplicates
            cases.push_back(v);
        }
        const std::size_t big = 100000;
        std::vector<std::uint64_t> sorted(big), reversed(big), equal(big, 7), pipe(big), few(big);
        for (std::size_t i = 0; i < big; ++i) {
            sorted[i] = i;
            reversed[i] = big - i;
            pipe[i] = i < big / 2 ? i : big - i;
            few[i] = rng.below(3);
        }
        for (auto *v : {&sorted, &reversed, &equal, &pipe, &few})
            cases.push_back(*v);
        for (std::vector<std::uint64_t> &v : cases) {
            std::vector<std::uint64_t> want = v;
            std::sort(want.begin(), want.end());
            introsort(v.data(), v.data() + v.size());
            QCOMPARE(v, want);
        }
    }

    void sortKeysMatchGolden()
    {
        std::vector<std::uint64_t> work(kSortKeys);
        for (std::uint64_t s = 0; s < kSeeds; ++s) {
            QCOMPARE(sortKeysUnit(s, work.data()), golden::kSortKeys[s]);
            if (s % 16 == 0) {
                // Exactly std::sort's result: sorted, and a permutation of the input.
                std::vector<std::uint64_t> want(kSortKeys);
                makeSortKeys(s, want.data());
                std::sort(want.begin(), want.end());
                QCOMPARE(work, want);
            }
        }
        SortKeysKernel k;
        QCOMPARE(k.makeThread(0)->unit(3 + kSeeds), golden::kSortKeys[3]);
    }

    void stringComparator()
    {
        // Strings in an arena, padded; refs made the way SortArena makes them.
        std::string arena;
        std::vector<StrRef> refs;
        const auto add = [&](const std::string &str) {
            StrRef r{};
            r.offset = std::uint32_t(arena.size());
            r.len = std::uint16_t(str.size());
            arena += str;
            char first8[8] = {};
            std::copy_n(str.begin(), std::min<std::size_t>(8, str.size()), first8);
            for (const char c : first8)
                r.prefixBE = (r.prefixBE << 8) | std::uint8_t(c);
            refs.push_back(r);
        };
        const std::vector<std::string> words = {"abcd", "abcde", "abcdefgh", "abcdefghi", "abcdefghij", "abcdefgha",
                                                "abcdefghijklmnop", "abcdefghijklmnoq", "abcdefghijklmnopq",
                                                "abcdefghijklmnopqrstuvwx", "abcdefghijklmnopqrstuvwy", "zzzz", "b"};
        for (const std::string &w : words)
            add(w);
        arena.append(8, '\0');
        for (std::size_t i = 0; i < words.size(); ++i) {
            for (std::size_t j = 0; j < words.size(); ++j)
                QVERIFY2(strLess(refs[i], refs[j], arena.data()) == (words[i] < words[j]),
                         qPrintable(QStringLiteral("%1 < %2").arg(QString::fromStdString(words[i]), QString::fromStdString(words[j]))));
        }
    }

    void sortStringsMatchGolden()
    {
        const SortArena arena;
        std::vector<StrRef> work(kSortStrings), input(kSortStrings);
        const auto text = [&](const StrRef &r) { return std::string(arena.text() + r.offset, r.len); };
        int pastFirstWord = 0;
        for (std::uint64_t s = 0; s < kSeeds; ++s) {
            QCOMPARE(sortStringsUnit(s, arena, work.data()), golden::kSortStrings[s]);
            if (s % 16 == 0) {
                makeSortStrings(s, arena, input.data());
                // The same order of contents as std::sort of the strings, and
                // a permutation of the input refs.
                std::vector<std::string> want, got;
                for (const StrRef &r : input)
                    want.push_back(text(r));
                std::sort(want.begin(), want.end());
                for (const StrRef &r : work)
                    got.push_back(text(r));
                QCOMPARE(got, want);
                std::vector<std::uint64_t> a, b;
                for (const StrRef &r : input)
                    a.push_back((std::uint64_t(r.offset) << 16) | r.len);
                for (const StrRef &r : work)
                    b.push_back((std::uint64_t(r.offset) << 16) | r.len);
                std::sort(a.begin(), a.end());
                std::sort(b.begin(), b.end());
                QCOMPARE(a, b);
                for (std::size_t i = 1; i < work.size(); ++i)
                    pastFirstWord += work[i].prefixBE == work[i - 1].prefixBE && work[i].len > 8 && work[i - 1].len > 8;
            }
        }
        // §5.6: neighbours that share their first 8 bytes are common, so the
        // comparator goes past the first word.
        qInfo("sorted neighbours sharing 8 bytes: %d of %d", pastFirstWord, int(4 * (kSortStrings - 1)));
        QVERIFY(pastFirstWord > int(kSortStrings));
        makeSortStrings(5, arena, input.data());
        for (const StrRef &r : input)
            QVERIFY(r.len >= 4 && r.len <= 24);
    }

    // §5.7, §14.1: the episode's result is bit-identical for 1, 2, 7 and all
    // threads (every episode is checked against the one-thread reference).
    void nbodyAnyThreadCount()
    {
        QCOMPARE(nbodyReferenceEpisode(), golden::kNbody);
        std::vector<int> counts = {1, 2, 7, logicalCpuCount()};
        for (const int n : counts) {
            const NbodyKernel k(n);
            WorkerPool pool({n, {}});
            const Measurement m = pool.measure(k, {10ms, 30ms, 2}, [] { return false; });
            QVERIFY2(m.ok(), qPrintable(QStringLiteral("%1 threads: %2").arg(n).arg(QString::fromStdString(m.error))));
            QVERIFY(m.reps[0].units >= 1);  // shared episodes
            qInfo("n-body on %d threads: %.0f M interactions/s", n, m.median / 1e6);
        }
        // A kernel made for another thread count can't run.
        const NbodyKernel k(3);
        WorkerPool pool({2, {}});
        QCOMPARE(pool.measure(k, {10ms, 10ms, 1}, [] { return false; }).outcome, Measurement::Outcome::Failed);
    }

    // Stop during an episode: every thread leaves at the same step barrier,
    // within 250 ms (an episode on a slow 4-thread CPU takes about 150 ms;
    // a step an eighth of that).
    void nbodyStopWithin250ms()
    {
        const int n = std::min(4, logicalCpuCount());
        const NbodyKernel k(n);
        WorkerPool pool({n, {}});
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
        QVERIFY2(took < 250ms, qPrintable(QStringLiteral("Stop took %1 ms").arg(msOf(took))));
        // The kernel is usable again (its state was reset).
        QVERIFY(pool.measure(k, {10ms, 20ms, 1}, [] { return false; }).ok());
    }

    // Each kernel through the engine: every unit checked against the golden
    // table (two threads, all seeds), a plausible rate.
    void kernelsThroughTheEngine()
    {
        const IntKernel i;
        const FpKernel f;
        const PrimesKernel p;
        const SortArena sortArena;
        const SortKeysKernel sk;
        const SortStringsKernel ss(sortArena);
        WorkerPool pool({2, {}});
        for (const Kernel *k : std::initializer_list<const Kernel *>{&i, &f, &p, &sk, &ss}) {
            const Measurement m = pool.measure(*k, {20ms, 50ms, 2}, [] { return false; });
            QVERIFY2(m.ok(), m.error.c_str());
            QVERIFY(m.median > 0);
            qInfo("%s: %.1f M/s on 2 threads", k->name().c_str(), m.median / 1e6);
        }
    }

    // §4.1: a unit takes at most 100 ms on the slowest supported CPU, so Stop
    // returns within 250 ms. Here: its CPU time (about 1-6 ms on Knowhere)
    // under 25 ms, which leaves a slow runner 4 times Knowhere's margin.
    void unitsAreShort()
    {
        const std::vector<std::uint32_t> base = primesBase();
        std::vector<std::uint64_t> bitset(kPrimesBitsetWords);
        const Clock::duration ti = cpuPerCall([] { intUnit(3); });
        const Clock::duration tf = cpuPerCall([] { fpUnit(3); });
        const Clock::duration tp = cpuPerCall([&] { primesUnit(3, base, bitset.data()); });
        const SimdInputs inputs;
        const SimdChoice c = chooseSimdPath(sb::isa::detect(), inputs);
        const simd::Input in = inputs.forSeed(3);
        simd::Output out{};
        const Clock::duration ts = cpuPerCall([&] { simdUnitFor(c.path)(&in, &out); });
        // The slowest path (SSE4.2: multiply + add, 4 lanes) on this CPU.
        const Clock::duration tsse = simdUnitFor(sb::isa::SimdPath::Sse42) && c.path != sb::isa::SimdPath::Sse42
                                         ? cpuPerCall([&] { simdUnitFor(sb::isa::SimdPath::Sse42)(&in, &out); })
                                         : ts;
        const SortArena sortArena;
        std::vector<std::uint64_t> keys(kSortKeys);
        std::vector<StrRef> strings(kSortStrings);
        const Clock::duration tk = cpuPerCall([&] { sortKeysUnit(3, keys.data()); });
        const Clock::duration tstr = cpuPerCall([&] { sortStringsUnit(3, sortArena, strings.data()); });
        qInfo("unit times: integer %.3f ms, FP %.3f ms, primes %.3f ms, SIMD %.3f ms (%s), SIMD %.3f ms (narrowest), "
              "integer sort %.3f ms, string sort %.3f ms",
              msOf(ti), msOf(tf), msOf(tp), msOf(ts), c.label.c_str(), msOf(tsse), msOf(tk), msOf(tstr));
        for (const Clock::duration t : {ti, tf, tp, ts, tsse, tk, tstr})
            QVERIFY(t < 25ms);
    }
};

QTEST_GUILESS_MAIN(TestCpuKernels)
#include "tst_cpu_kernels.moc"
