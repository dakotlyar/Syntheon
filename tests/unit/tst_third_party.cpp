// SPDX-License-Identifier: GPL-3.0-or-later
// The pinned compression libraries (Phase 2 design §11): each one links,
// reports exactly the pinned version, and round-trips a buffer. The CPU test's
// own golden checks come later (task 10); this proves the build.
#include <lz4.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include <QByteArray>
#include <QTest>

#include <cstdint>
#include <vector>

namespace {

// 256 KiB of mixed data: repetitive text with a pseudo-random tail, so every
// codec actually has something to compress and something it can't.
QByteArray sample()
{
    QByteArray data;
    data.reserve(256 * 1024);
    const QByteArray phrase("SynBench compresses the same block on every machine. ");
    while (data.size() < 192 * 1024)
        data.append(phrase);
    std::uint64_t x = 0x9E3779B97F4A7C15ull;
    while (data.size() < 256 * 1024) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        data.append(char(x & 0xFF));
    }
    return data;
}

} // namespace

class TestThirdParty : public QObject
{
    Q_OBJECT

private slots:
    void pinnedVersions()
    {
        QCOMPARE(QByteArray(zlibVersion()), QByteArray("1.3.2"));
        QCOMPARE(QByteArray(ZSTD_versionString()), QByteArray("1.5.7"));
        QCOMPARE(QByteArray(LZ4_versionString()), QByteArray("1.10.0"));
        QCOMPARE(QByteArray(lzma_version_string()), QByteArray("5.8.4"));
    }

    void zlibRoundTrip()
    {
        const QByteArray in = sample();
        uLongf packedSize = compressBound(uLong(in.size()));
        std::vector<Bytef> packed(packedSize);
        QCOMPARE(compress2(packed.data(), &packedSize, reinterpret_cast<const Bytef *>(in.constData()), uLong(in.size()), 6), Z_OK);
        QVERIFY(packedSize < uLongf(in.size()));
        QByteArray out(in.size(), '\0');
        uLongf outSize = uLongf(out.size());
        QCOMPARE(uncompress(reinterpret_cast<Bytef *>(out.data()), &outSize, packed.data(), packedSize), Z_OK);
        QCOMPARE(outSize, uLongf(in.size()));
        QCOMPARE(out, in);
        QCOMPARE(crc32(0, reinterpret_cast<const Bytef *>(out.constData()), uInt(out.size())),
                 crc32(0, reinterpret_cast<const Bytef *>(in.constData()), uInt(in.size())));
    }

    void zstdRoundTrip()
    {
        const QByteArray in = sample();
        std::vector<char> packed(ZSTD_compressBound(size_t(in.size())));
        const size_t packedSize = ZSTD_compress(packed.data(), packed.size(), in.constData(), size_t(in.size()), 3);
        QVERIFY2(!ZSTD_isError(packedSize), ZSTD_getErrorName(packedSize));
        QVERIFY(packedSize < size_t(in.size()));
        QByteArray out(in.size(), '\0');
        const size_t outSize = ZSTD_decompress(out.data(), size_t(out.size()), packed.data(), packedSize);
        QVERIFY2(!ZSTD_isError(outSize), ZSTD_getErrorName(outSize));
        QCOMPARE(outSize, size_t(in.size()));
        QCOMPARE(out, in);
    }

    void lz4RoundTrip()
    {
        const QByteArray in = sample();
        std::vector<char> packed(size_t(LZ4_compressBound(int(in.size()))));
        const int packedSize = LZ4_compress_default(in.constData(), packed.data(), int(in.size()), int(packed.size()));
        QVERIFY(packedSize > 0);
        QVERIFY(packedSize < int(in.size()));
        QByteArray out(in.size(), '\0');
        QCOMPARE(LZ4_decompress_safe(packed.data(), out.data(), packedSize, int(out.size())), int(in.size()));
        QCOMPARE(out, in);
    }

    // xz preset 6 with the dictionary capped at the block size (design P11).
    void xzRoundTrip()
    {
        const QByteArray in = sample();
        lzma_options_lzma opt;
        QVERIFY(!lzma_lzma_preset(&opt, 6));
        opt.dict_size = 256 * 1024;
        lzma_filter filters[] = {{LZMA_FILTER_LZMA2, &opt}, {LZMA_VLI_UNKNOWN, nullptr}};
        std::vector<std::uint8_t> packed(lzma_stream_buffer_bound(size_t(in.size())));
        size_t packedSize = 0;
        QCOMPARE(lzma_stream_buffer_encode(filters, LZMA_CHECK_CRC32, nullptr, reinterpret_cast<const std::uint8_t *>(in.constData()),
                                           size_t(in.size()), packed.data(), &packedSize, packed.size()),
                 LZMA_OK);
        QVERIFY(packedSize < size_t(in.size()));
        QByteArray out(in.size(), '\0');
        std::uint64_t memlimit = UINT64_MAX;
        size_t inPos = 0, outPos = 0;
        QCOMPARE(lzma_stream_buffer_decode(&memlimit, 0, nullptr, packed.data(), &inPos, packedSize,
                                           reinterpret_cast<std::uint8_t *>(out.data()), &outPos, size_t(out.size())),
                 LZMA_OK);
        QCOMPARE(outPos, size_t(in.size()));
        QCOMPARE(out, in);
    }
};

QTEST_GUILESS_MAIN(TestThirdParty)
#include "tst_third_party.moc"
