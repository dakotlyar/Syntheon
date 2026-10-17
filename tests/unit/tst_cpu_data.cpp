// SPDX-License-Identifier: GPL-3.0-or-later
// The CPU test's deterministic data (Phase 2 design §5.4, §6.4, P10): the
// PRNG against an independent implementation, and the compression corpus
// against its golden CRC (the same committed value on every platform).
#include "tests/cpu/compress/corpus.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/prng.h"

#include <QTest>

#include <zlib.h>

using namespace sb::cpu;

class TestCpuData : public QObject
{
    Q_OBJECT

private slots:
    // Known answers computed with an independent Python implementation of
    // splitmix64 / xoshiro256** (2026-10-16); splitmix64(0)'s first output is
    // the published reference value 0xe220a8397b1dcdaf.
    void prngKnownAnswers()
    {
        SplitMix64 sm(0);
        QCOMPARE(sm.next(), 0xe220a8397b1dcdafull);
        QCOMPARE(sm.next(), 0x6e789e6aa1b965f4ull);
        QCOMPARE(sm.next(), 0x06c45d188009454full);

        Xoshiro256ss a(0);
        QCOMPARE(a.next(), 0x99ec5f36cb75f2b4ull);
        QCOMPARE(a.next(), 0xbf6e1f784956452aull);
        QCOMPARE(a.next(), 0x1a5f849d4933e6e0ull);
        QCOMPARE(a.next(), 0x6aa594f1262d2d2cull);
        Xoshiro256ss b(0x123456789abcdefull);
        QCOMPARE(b.next(), 0xa2c2a42038d4ec3dull);
        QCOMPARE(b.next(), 0x05fc25d0738e7b0full);
    }

    void prngBelow()
    {
        Xoshiro256ss r(42);
        const std::uint64_t small[] = {0, 3, 6, 9, 9, 7, 7, 8, 7, 5, 6, 2};
        for (std::uint64_t want : small)
            QCOMPARE(r.below(10), want);
        Xoshiro256ss r2(42);
        QCOMPARE(r2.below(1000003), 83863ull);
        QCOMPARE(r2.below(1000003), 378981ull);
        Xoshiro256ss r3(7);
        QCOMPARE(r3.below(1ull << 63), 0x59ac7d7ba77cbb2dull);  // the rejection path's range
        // Always in range, every value reachable.
        Xoshiro256ss r4(1);
        int seen[7] = {};
        for (int i = 0; i < 7000; ++i) {
            const std::uint64_t v = r4.below(7);
            QVERIFY(v < 7);
            ++seen[v];
        }
        for (int c : seen)
            QVERIFY(c > 800 && c < 1200);
    }

    void corpusMatchesGolden()
    {
        const std::vector<std::uint8_t> corpus = generateCorpus(4);
        QCOMPARE(corpus.size(), kCorpusSize);
        QCOMPARE(kCorpusSize, std::size_t(32) * 1024 * 1024);
        QCOMPARE(crc32Of(corpus), golden::kCorpusCrc32);
    }

    // The corpus doesn't depend on how many threads made it, and a block is
    // the same made alone.
    void corpusIndependentOfThreads()
    {
        const std::vector<std::uint8_t> one = generateCorpus(1);
        QVERIFY(one == generateCorpus(7));
        std::vector<std::uint8_t> block(kCorpusBlockSize);
        generateCorpusBlock(77, block);
        QVERIFY(std::equal(block.begin(), block.end(), one.begin() + 77 * kCorpusBlockSize));
    }

    // The mix (§5.4) is what it claims: text compresses well, the random tail
    // not at all, the whole block in between.
    void corpusMix()
    {
        std::vector<std::uint8_t> block(kCorpusBlockSize);
        generateCorpusBlock(3, block);
        const auto ratio = [](const std::uint8_t *p, std::size_t n) {
            uLongf size = compressBound(uLong(n));
            std::vector<Bytef> out(size);
            compress2(out.data(), &size, p, uLong(n), 6);
            return double(size) / double(n);
        };
        const std::size_t text = kCorpusBlockSize / 2, random = kCorpusBlockSize - text - 2 * (kCorpusBlockSize / 5);
        QVERIFY(ratio(block.data(), text) < 0.4);
        QVERIFY(ratio(block.data() + kCorpusBlockSize - random, random) > 0.99);
        const double whole = ratio(block.data(), block.size());
        QVERIFY(whole > 0.3 && whole < 0.5);
        // Text is printable ASCII and line breaks.
        for (std::size_t i = 0; i < text; ++i)
            QVERIFY(block[i] == '\n' || (block[i] >= 0x20 && block[i] < 0x7F));
    }

    void crc32OfMatchesZlib()
    {
        const std::uint8_t abc[] = {'a', 'b', 'c'};
        QCOMPARE(crc32Of(abc), 0x352441C2u);
        QCOMPARE(crc32Of({}), 0u);
    }
};

QTEST_GUILESS_MAIN(TestCpuData)
#include "tst_cpu_data.moc"
