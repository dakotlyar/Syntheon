// SPDX-License-Identifier: GPL-3.0-or-later
// The compression sub-test (Phase 2 design §5.4, §14.1): every block of every
// codec round-trips and has its golden compressed size, streaming in 64 KiB
// pieces gives the libraries' one-call output, the prep step, the kernels
// through the engine (a corrupt block is an Error), Stop in the middle of xz
// within 250 ms, and the single-thread mixed unit.
#include "tests/cpu/compress/compress_kernels.h"
#include "tests/cpu/engine/platform_threads.h"
#include "tests/cpu/engine/worker_pool.h"
#include "tests/cpu/golden/golden.h"

#include <QTest>

#include <lz4.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <stdexcept>
#include <thread>

using namespace sb::cpu;
using namespace std::chrono_literals;

namespace {

const CorpusData &corpus()
{
    static const CorpusData c = makeCorpusData(std::max(1, std::min(8, logicalCpuCount())));
    return c;
}

// What each library gives in one call, with the same settings.
std::vector<std::uint8_t> oneShot(Codec codec, std::span<const std::uint8_t> in)
{
    std::vector<std::uint8_t> out(CodecContext::bound(codec, in.size()));
    std::size_t n = 0;
    switch (codec) {
    case Codec::Zlib: {
        uLongf len = uLongf(out.size());
        if (compress2(out.data(), &len, in.data(), uLong(in.size()), 6) == Z_OK)
            n = len;
        break;
    }
    case Codec::Zstd: n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3); break;
    case Codec::Lz4:
        n = std::size_t(LZ4_compress_default(reinterpret_cast<const char *>(in.data()),
                                             reinterpret_cast<char *>(out.data()), int(in.size()), int(out.size())));
        break;
    case Codec::Xz: {
        lzma_options_lzma o;
        lzma_lzma_preset(&o, 6);
        o.dict_size = 256 * 1024;
        lzma_filter f[2] = {{LZMA_FILTER_LZMA2, &o}, {LZMA_VLI_UNKNOWN, nullptr}};
        // The same stream encoder in one call (lzma_stream_buffer_encode()
        // writes a different container: the block header carries the sizes).
        lzma_stream s = LZMA_STREAM_INIT;
        if (lzma_stream_encoder(&s, f, LZMA_CHECK_CRC64) == LZMA_OK) {
            s.next_in = in.data();
            s.avail_in = in.size();
            s.next_out = out.data();
            s.avail_out = out.size();
            if (lzma_code(&s, LZMA_FINISH) == LZMA_STREAM_END)
                n = std::size_t(s.total_out);
        }
        lzma_end(&s);
        break;
    }
    }
    out.resize(n);
    return out;
}

} // namespace

class TestCpuCompress : public QObject
{
    Q_OBJECT

private slots:
    void corpusData()
    {
        const CorpusData &c = corpus();
        QCOMPARE(c.bytes.size(), kCorpusSize);
        QCOMPARE(c.crc, golden::kCorpusCrc32);
        QCOMPARE(c.crc, crc32Of(c.bytes));
        QCOMPARE(c.blockCrc[5], crc32Of(c.block(5)));
        QCOMPARE(c.blockHash[5], blockHash(c.block(5)));
    }

    // §5.4: the decompression check's hash, against an independent Python
    // implementation (integers only: the same on every compiler).
    void blockHashKnownAnswers()
    {
        std::uint8_t bytes[100];
        for (int i = 0; i < 100; ++i)
            bytes[i] = std::uint8_t(i);
        QCOMPARE(blockHash(bytes), 0x67003CCCB003A004ull);  // three full 32-byte steps and a 4-byte tail
        QCOMPARE(blockHash(corpus().block(0)), 0x7CF69545590FD82Bull);
        QCOMPARE(corpus().blockHash[0], 0x7CF69545590FD82Bull);
        std::vector<std::uint8_t> changed(corpus().block(0).begin(), corpus().block(0).end());
        changed[123456] ^= 0x10;
        QVERIFY(blockHash(changed) != corpus().blockHash[0]);
    }

    void labels()
    {
        QCOMPARE(QString::fromLatin1(codecKey(Codec::Lz4)), QStringLiteral("lz4"));
        QCOMPARE(QString::fromLatin1(codecLabel(Codec::Xz)), QStringLiteral("xz (preset 6)"));
#if defined(NDEBUG)
        QVERIFY(codecsOptimised());
#endif
    }

    // Every block of every codec: the golden compressed size, and the round trip.
    void roundTripAndGoldenSizes_data()
    {
        QTest::addColumn<int>("codec");
        for (const Codec c : kCodecs)
            QTest::addRow("%s", codecName(c)) << int(c);
    }
    void roundTripAndGoldenSizes()
    {
        QFETCH(int, codec);
        const Codec c = Codec(codec);
        // Compressed by the prep step on every CPU (xz alone takes about 6 s
        // on one), then each block checked here.
        const std::optional<CompressedSet> set = compressCorpus(c, corpus(), logicalCpuCount(), {});
        QVERIFY(set);
        QCOMPARE(set->sizeMismatches, 0);
        CodecContext ctx(c);
        std::vector<std::uint8_t> back(kCorpusBlockSize);
        for (std::size_t b = 0; b < kCorpusBlocks; ++b) {
            QCOMPARE(std::uint32_t(set->blocks[b].size()), goldenCompressedSize(c, b));
            ctx.decompress(set->blocks[b], back);
            QVERIFY(std::equal(back.begin(), back.end(), corpus().block(b).begin()));
        }
        qInfo("%s: ratio %.2f", codecLabel(c), set->ratio());
        QVERIFY(set->ratio() > 1.5 && set->ratio() < 6);
    }

    // Streaming in 64 KiB pieces changes nothing: the output is what the
    // library gives in one call with the same settings and API.
    void streamingEqualsOneShot_data() { roundTripAndGoldenSizes_data(); }
    void streamingEqualsOneShot()
    {
        QFETCH(int, codec);
        const Codec c = Codec(codec);
        CodecContext ctx(c);
        std::vector<std::uint8_t> packed(CodecContext::bound(c, kCorpusBlockSize));
        for (const std::size_t b : {0, 1, 77, 127}) {
            const std::size_t n = *ctx.compress(corpus().block(b), packed);
            const std::vector<std::uint8_t> ref = oneShot(c, corpus().block(b));
            QCOMPARE(n, ref.size());
            QVERIFY(std::equal(ref.begin(), ref.end(), packed.begin()));
        }
    }

    void corruptStreamThrows()
    {
        CodecContext ctx(Codec::Zstd);
        std::vector<std::uint8_t> packed(CodecContext::bound(Codec::Zstd, kCorpusBlockSize)), back(kCorpusBlockSize);
        const std::size_t n = *ctx.compress(corpus().block(3), packed);
        packed[0] ^= 0xFF;  // the frame's magic number
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ctx.decompress(std::span(packed).first(n), back));
    }

    // The prep step: the same set however many threads made it.
    void compressCorpusInParallel()
    {
        const std::optional<CompressedSet> s4 = compressCorpus(Codec::Lz4, corpus(), 4, {});
        const std::optional<CompressedSet> s1 = compressCorpus(Codec::Lz4, corpus(), 1, {});
        QVERIFY(s4 && s1);
        QCOMPARE(s4->blocks, s1->blocks);
        QCOMPARE(s4->compressedBytes, s1->compressedBytes);
    }

    void compressCorpusCancels()
    {
        const auto t0 = Clock::now();
        const std::optional<CompressedSet> s = compressCorpus(Codec::Xz, corpus(), 1, [&] { return Clock::now() - t0 > 100ms; });
        QVERIFY(!s);
        QVERIFY2(Clock::now() - t0 < 350ms, "the prep step didn't stop");
    }

    // All eight kernels through the engine on two threads: every unit
    // checked, the sizes the golden ones.
    void kernelsThroughTheEngine()
    {
        WorkerPool pool({2, {}});
        for (const Codec c : kCodecs) {
            const CompressKernel ck(c, corpus());
            const Measurement mc = pool.measure(ck, {20ms, 60ms, 2}, [] { return false; });
            QVERIFY2(mc.ok(), mc.error.c_str());
            QCOMPARE(ck.sizeMismatches(), 0);
            const std::optional<CompressedSet> set = compressCorpus(c, corpus(), 2, {});
            QVERIFY(set);
            const DecompressKernel dk(corpus(), *set);
            const Measurement md = pool.measure(dk, {20ms, 60ms, 2}, [] { return false; });
            QVERIFY2(md.ok(), md.error.c_str());
            qInfo("%s: compress %.1f MB/s, decompress %.1f MB/s on 2 threads", codecLabel(c), mc.median / 1e6,
                  md.median / 1e6);
        }
    }

    // §5.4: a block that decompresses to something else is an Error (here a
    // flipped literal byte in LZ4's output, which LZ4 itself accepts).
    void wrongDecompressionIsAnError()
    {
        std::optional<CompressedSet> set = compressCorpus(Codec::Lz4, corpus(), 2, {});
        QVERIFY(set);
        // The last byte of an LZ4 block is always a literal.
        for (std::vector<std::uint8_t> &b : set->blocks)
            b.back() ^= 0x01;
        const DecompressKernel dk(corpus(), *set);
        WorkerPool pool({1, {}});
        const Measurement m = pool.measure(dk, {10ms, 10ms, 1}, [] { return false; });
        QCOMPARE(m.outcome, Measurement::Outcome::WrongResult);
        QVERIFY(QString::fromStdString(m.error).startsWith(QLatin1StringView("LZ4 decompression kernel produced a wrong result")));
    }

    // §14.1: Stop in the middle of xz returns within 250 ms (the codec checks
    // between 64 KiB pieces), also while a unit runs past the deadline.
    void stopMidXzWithin250ms()
    {
        const CompressKernel k(Codec::Xz, corpus());
        WorkerPool pool({2, {}});
        for (const auto when : {350ms, 125ms}) {  // in a repetition; in the warm-up's drain
            std::atomic<bool> cancel{false};
            Clock::time_point cancelledAt{};
            std::thread canceller([&] {
                std::this_thread::sleep_for(when);
                cancelledAt = Clock::now();
                cancel = true;
            });
            const Measurement m = pool.measure(k, {100ms, 10000ms, 5}, [&] { return cancel.load(); });
            const auto took = Clock::now() - cancelledAt;
            canceller.join();
            QCOMPARE(m.outcome, Measurement::Outcome::Cancelled);
            QVERIFY2(took < 250ms,
                     qPrintable(QStringLiteral("Stop took %1 ms")
                                    .arg(std::chrono::duration<double, std::milli>(took).count())));
        }
        // And one piece is short: an xz compression of 64 KiB, timed here.
        CodecContext ctx(Codec::Xz);
        std::vector<std::uint8_t> out(CodecContext::bound(Codec::Xz, kCorpusBlockSize));
        const auto t0 = Clock::now();
        ctx.compress(corpus().block(0), out);
        const double perPiece = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / 4;
        qInfo("xz: %.1f ms per 64 KiB piece", perPiece);
    }

    // The single-thread mixed unit (§5.4, single.compress): its rate is the
    // geometric mean of the 8 calls, between the slowest and the fastest.
    void singleThreadMix()
    {
        const SingleCompressKernel k(corpus());
        WorkerPool pool({1, {}});
        const Measurement m = pool.measure(k, {50ms, 300ms, 2}, [] { return false; });
        QVERIFY2(m.ok(), m.error.c_str());
        QCOMPARE(k.sizeMismatches(), 0);
        qInfo("single-thread compression: %.1f MB/s (geometric mean of 8)", m.median / 1e6);
        // xz compression (about 5 MB/s) < the mean < LZ4 decompression (GB/s).
        QVERIFY(m.median > 3e6 && m.median < 3e9);
        // The unit through the golden: four round trips of block 9.
        QCOMPARE(k.makeThread(0)->unit(9 + 128), *k.golden(9));
    }

    void codecMemoryPerThread()
    {
        lzma_options_lzma o;
        lzma_lzma_preset(&o, 6);
        o.dict_size = 256 * 1024;
        lzma_filter f[2] = {{LZMA_FILTER_LZMA2, &o}, {LZMA_VLI_UNKNOWN, nullptr}};
        const std::uint64_t xz = lzma_raw_encoder_memusage(f);
        qInfo("xz encoder: %.1f MiB per thread", double(xz) / (1 << 20));
        QVERIFY(xz < 8u << 20);  // "a few MiB per thread" (§5.4)
    }
};

QTEST_GUILESS_MAIN(TestCpuCompress)
#include "tst_cpu_compress.moc"
