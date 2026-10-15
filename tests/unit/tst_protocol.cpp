// SPDX-License-Identifier: GPL-3.0-or-later
#include "privileged/protocol/protocol.h"

#include <QRandomGenerator>
#include <QTest>

using namespace sb::proto;

namespace {

Bytes bytes(std::initializer_list<int> v)
{
    Bytes b;
    for (int x : v)
        b.push_back(std::uint8_t(x));
    return b;
}

void feed(Decoder &d, const Bytes &b)
{
    d.feed(b.data(), b.size());
}

} // namespace

class TestProtocol : public QObject
{
    Q_OBJECT

private slots:
    void wireLayoutIsLittleEndian()
    {
        Frame f;
        f.requestId = 0x01020304;
        f.op = std::uint16_t(Op::ReadSmbios);
        f.flags = Response;
        f.payload = bytes({0xAA, 0xBB});
        // length = 8 + 2, request id, op, flags (version in the high byte), payload
        QCOMPARE(encode(f), bytes({10, 0, 0, 0, 4, 3, 2, 1, 3, 0, 1, kProtocolVersion, 0xAA, 0xBB}));
    }

    void roundTripAcrossChunks()
    {
        Bytes stream;
        for (std::uint32_t i = 1; i <= 3; ++i) {
            Frame f;
            f.requestId = i;
            f.op = std::uint16_t(Op::Ping);
            f.payload = Bytes(i * 5, std::uint8_t(i));
            const Bytes e = encode(f);
            stream.insert(stream.end(), e.begin(), e.end());
        }
        // One byte at a time: frames come out whole and in order.
        Decoder d(kMaxRequestFrame);
        std::vector<Frame> got;
        for (std::uint8_t b : stream) {
            d.feed(&b, 1);
            Frame out;
            while (d.next(out) == Decoder::Status::Frame)
                got.push_back(out);
        }
        QCOMPARE(got.size(), size_t(3));
        QCOMPARE(got[2].requestId, 3u);
        QCOMPARE(got[2].payload, Bytes(15, 3));
        QCOMPARE(got[0].version, kProtocolVersion);
        QCOMPARE(d.buffered(), size_t(0));
    }

    void truncatedNeedsMore()
    {
        Frame f;
        f.op = std::uint16_t(Op::Ping);
        f.payload = Bytes(10, 1);
        const Bytes e = encode(f);
        Decoder d(kMaxRequestFrame);
        d.feed(e.data(), e.size() - 1);
        Frame out;
        QCOMPARE(d.next(out), Decoder::Status::NeedMore);
        d.feed(e.data() + e.size() - 1, 1);
        QCOMPARE(d.next(out), Decoder::Status::Frame);
    }

    void oversizedIsReportedThenSkipped()
    {
        Frame big;
        big.requestId = 7;
        big.op = std::uint16_t(Op::Ping);
        big.payload = Bytes(kMaxRequestFrame, 0x55);  // header pushes it over 4 KiB
        Frame small;
        small.requestId = 8;
        small.op = std::uint16_t(Op::Ping);
        Bytes stream = encode(big);
        const Bytes s = encode(small);
        stream.insert(stream.end(), s.begin(), s.end());

        Decoder d(kMaxRequestFrame);
        // Arrives in pieces, so part of the oversized frame is skipped later.
        feed(d, Bytes(stream.begin(), stream.begin() + 100));
        Frame out;
        QCOMPARE(d.next(out), Decoder::Status::Oversized);
        QCOMPARE(out.requestId, 7u);
        QCOMPARE(d.next(out), Decoder::Status::NeedMore);
        feed(d, Bytes(stream.begin() + 100, stream.end()));
        QCOMPARE(d.next(out), Decoder::Status::Frame);  // back in sync
        QCOMPARE(out.requestId, 8u);
    }

    void hugeClaimedLengthDoesntBuffer()
    {
        Decoder d(kMaxRequestFrame);
        feed(d, bytes({0xFF, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0, 2, 0, 1, 1}));
        Frame out;
        QCOMPARE(d.next(out), Decoder::Status::Oversized);
        feed(d, Bytes(64 * 1024, 0));  // swallowed, not stored
        QCOMPARE(d.buffered(), size_t(0));
    }

    void lengthTooSmallForHeader()
    {
        Decoder d(kMaxRequestFrame);
        feed(d, bytes({3, 0, 0, 0, 9, 9, 9}));
        Frame out;
        QCOMPARE(d.next(out), Decoder::Status::Oversized);  // malformed framing is dropped the same way
        QCOMPARE(d.buffered(), size_t(0));
    }

    void versionTravelsInFlags()
    {
        Frame f;
        f.op = std::uint16_t(Op::Ping);
        f.version = 9;
        f.flags = Response | Error;
        const Bytes e = encode(f);
        Decoder d(kMaxResponseFrame);
        feed(d, e);
        Frame out;
        QCOMPARE(d.next(out), Decoder::Status::Frame);
        QCOMPARE(out.version, std::uint8_t(9));
        QVERIFY(out.isResponse());
        QVERIFY(out.isError());
    }

    void helloPayload()
    {
        const Hello h{kProtocolVersion, 0, "linux-x64 abc1234"};
        const auto back = decodeHello(encodeHello(h));
        QVERIFY(back);
        QCOMPARE(back->protocolVersion, std::uint32_t(kProtocolVersion));
        QCOMPARE(back->euid, 0u);
        QCOMPARE(back->buildId, std::string("linux-x64 abc1234"));
        Bytes bad = encodeHello(h);
        bad.push_back(0);  // trailing byte
        QVERIFY(!decodeHello(bad));
        bad.resize(bad.size() - 3);  // short
        QVERIFY(!decodeHello(bad));
    }

    void smbiosPayload()
    {
        const SmbiosBlobs s{Bytes{'_', 'S', 'M', '3', '_'}, Bytes(3000, 0x11)};
        const auto back = decodeSmbios(encodeSmbios(s));
        QVERIFY(back);
        QCOMPARE(back->entryPoint, s.entryPoint);
        QCOMPARE(back->table, s.table);
        Bytes bad = encodeSmbios(s);
        bad[0] = 0xFF;  // entry point length past the end
        QVERIFY(!decodeSmbios(bad));
        QVERIFY(!decodeSmbios(Bytes{}));
    }

    void errorPayloadAndHelpers()
    {
        Frame req;
        req.requestId = 42;
        req.op = 99;
        const Frame e = makeError(req.requestId, req.op, ErrorCode::UnknownOp, "unknown op 99");
        QVERIFY(e.isResponse() && e.isError());
        QCOMPARE(e.requestId, 42u);
        const auto back = decodeError(e.payload);
        QVERIFY(back);
        QCOMPARE(back->code, ErrorCode::UnknownOp);
        QCOMPARE(back->message, std::string("unknown op 99"));
        const Frame r = makeResponse(req, Bytes{1});
        QCOMPARE(r.requestId, 42u);
        QCOMPARE(r.flags, std::uint8_t(Response));
        QVERIFY(!decodeError(Bytes{1}));
    }

    void randomBytesNeverCrash()
    {
        QRandomGenerator rng(1234);
        for (int round = 0; round < 200; ++round) {
            Bytes junk(rng.bounded(1, 300));
            for (auto &b : junk)
                b = std::uint8_t(rng.bounded(256));
            Decoder d(kMaxRequestFrame);
            feed(d, junk);
            Frame out;
            for (int i = 0; i < 100 && d.next(out) != Decoder::Status::NeedMore; ++i) {
                decodeHello(out.payload);
                decodeSmbios(out.payload);
                decodeError(out.payload);
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestProtocol)
#include "tst_protocol.moc"
