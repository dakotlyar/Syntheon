// SPDX-License-Identifier: GPL-3.0-or-later
#include "helper/helper_core.h"

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

using namespace sb;
using namespace sb::proto;

namespace {

Bytes request(Op op, std::uint32_t id = 1, Bytes payload = {}, std::uint8_t flags = 0,
              std::uint8_t version = kProtocolVersion)
{
    Frame f;
    f.requestId = id;
    f.op = std::uint16_t(op);
    f.flags = flags;
    f.version = version;
    f.payload = std::move(payload);
    return encode(f);
}

Frame decodeOne(const Bytes &b)
{
    Decoder d(kMaxResponseFrame);
    d.feed(b.data(), b.size());
    Frame f;
    [&] { QCOMPARE(d.next(f), Decoder::Status::Frame); }();
    return f;
}

ErrorCode errorOf(const Bytes &reply)
{
    const Frame f = decodeOne(reply);
    const auto e = decodeError(f.payload);
    return f.isError() && e ? e->code : ErrorCode(0);
}

QString helperBinary()
{
    return QStringLiteral(SB_HELPER_PATH);
}

} // namespace

class TestHelperCore : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_entry, m_table;

    helper::Core core()
    {
        helper::Config c;
        c.entryPointPath = m_entry.toStdString();
        c.tablePath = m_table.toStdString();
        c.buildId = "2026.11.0-dev+abc linux-x64 abc";
        c.euid = 1000;
        return helper::Core(c);
    }

    helper::Core::Output feed(helper::Core &c, const Bytes &b) { return c.feed(b.data(), b.size()); }

private slots:
    void initTestCase()
    {
        m_entry = m_dir.filePath(QStringLiteral("smbios_entry_point"));
        m_table = m_dir.filePath(QStringLiteral("DMI"));
        QFile e(m_entry);
        QVERIFY(e.open(QIODevice::WriteOnly));
        e.write(QByteArray("_SM3_\x00\x18\x03\x06", 9));
        e.close();
        QFile t(m_table);
        QVERIFY(t.open(QIODevice::WriteOnly));
        t.write(QByteArray(3887, 'x'));
    }

    void hello()
    {
        const Frame f = decodeOne(core().hello());
        QCOMPARE(f.op, std::uint16_t(Op::Hello));
        QCOMPARE(f.requestId, 0u);
        QVERIFY(f.isResponse());
        const auto h = decodeHello(f.payload);
        QVERIFY(h);
        QCOMPARE(h->euid, 1000u);
        QCOMPARE(h->buildId, std::string("2026.11.0-dev+abc linux-x64 abc"));
    }

    void pingAndShutdown()
    {
        auto c = core();
        auto out = feed(c, request(Op::Ping, 5));
        QCOMPARE(out.replies.size(), size_t(1));
        QVERIFY(!out.exit);
        QCOMPARE(decodeOne(out.replies[0]).requestId, 5u);
        out = feed(c, request(Op::Shutdown, 6));
        QVERIFY(out.exit);
        QCOMPARE(out.exitCode, 0);
        QCOMPARE(decodeOne(out.replies[0]).requestId, 6u);  // answered before exiting
    }

    void readSmbios()
    {
        auto c = core();
        const auto out = feed(c, request(Op::ReadSmbios, 9));
        const Frame f = decodeOne(out.replies[0]);
        QVERIFY(!f.isError());
        const auto blobs = decodeSmbios(f.payload);
        QVERIFY(blobs);
        QCOMPARE(blobs->entryPoint.size(), size_t(9));
        QCOMPARE(blobs->table.size(), size_t(3887));
    }

    void readSmbiosErrors()
    {
        helper::Config cfg;
        cfg.entryPointPath = m_dir.filePath(QStringLiteral("missing")).toStdString();
        cfg.tablePath = m_table.toStdString();
        helper::Core missing(cfg);
        QCOMPARE(errorOf(missing.feed(request(Op::ReadSmbios).data(), request(Op::ReadSmbios).size()).replies[0]),
                 ErrorCode::IoError);

        // A symlink is not followed (O_NOFOLLOW).
        const QString link = m_dir.filePath(QStringLiteral("link"));
        QVERIFY(QFile::link(m_entry, link));
        cfg.entryPointPath = link.toStdString();
        helper::Core linked(cfg);
        const Bytes r = request(Op::ReadSmbios);
        QCOMPARE(errorOf(linked.feed(r.data(), r.size()).replies[0]), ErrorCode::IoError);

        // Too big for a response frame.
        const QString big = m_dir.filePath(QStringLiteral("big"));
        QFile b(big);
        QVERIFY(b.open(QIODevice::WriteOnly));
        b.write(QByteArray(5 * 1024 * 1024, 'x'));
        b.close();
        cfg.entryPointPath = m_entry.toStdString();
        cfg.tablePath = big.toStdString();
        helper::Core huge(cfg);
        QCOMPARE(errorOf(huge.feed(r.data(), r.size()).replies[0]), ErrorCode::IoError);

        // Permission denied -> NotPermitted (what a non-root helper gets).
        if (geteuid() != 0) {
            const QString locked = m_dir.filePath(QStringLiteral("locked"));
            QFile l(locked);
            QVERIFY(l.open(QIODevice::WriteOnly));
            l.close();
            QFile::setPermissions(locked, QFileDevice::Permissions());
            cfg.tablePath = locked.toStdString();
            helper::Core denied(cfg);
            QCOMPARE(errorOf(denied.feed(r.data(), r.size()).replies[0]), ErrorCode::NotPermitted);
        }
    }

    void rejectsBadRequests()
    {
        auto c = core();
        QCOMPARE(errorOf(feed(c, request(Op(99))).replies[0]), ErrorCode::UnknownOp);
        auto c2 = core();
        QCOMPARE(errorOf(feed(c2, request(Op::Ping, 1, {}, 0, 7)).replies[0]), ErrorCode::BadVersion);
        auto c3 = core();
        QCOMPARE(errorOf(feed(c3, request(Op::Ping, 1, {}, Response)).replies[0]), ErrorCode::Malformed);
        auto c4 = core();
        QCOMPARE(errorOf(feed(c4, request(Op::Ping, 1, Bytes{1, 2})).replies[0]), ErrorCode::Malformed);
        auto c5 = core();
        QCOMPARE(errorOf(feed(c5, request(Op::Hello)).replies[0]), ErrorCode::UnknownOp);  // GUI can't send HELLO
    }

    void oversizedRequestThenRecovers()
    {
        auto c = core();
        Bytes stream = request(Op::Ping, 1, Bytes(kMaxRequestFrame, 0));
        const Bytes ping = request(Op::Ping, 2);
        stream.insert(stream.end(), ping.begin(), ping.end());
        const auto out = feed(c, stream);
        QCOMPARE(out.replies.size(), size_t(2));
        QCOMPARE(errorOf(out.replies[0]), ErrorCode::Oversized);
        QVERIFY(!decodeOne(out.replies[1]).isError());
        QVERIFY(!out.exit);
    }

    void threeBadFramesInARowExit()
    {
        auto c = core();
        // bad, bad, good, bad, bad: the good one resets the count.
        for (Op op : {Op(99), Op(98), Op::Ping, Op(97), Op(96)})
            QVERIFY(!feed(c, request(op)).exit);
        const auto out = feed(c, request(Op(95)));
        QVERIFY(out.exit);
        QCOMPARE(out.exitCode, helper::Core::kExitTooManyBadFrames);
    }

    // --- the real synbench-helper binary, as this (non-root) user ---

    void binaryRefusesArgumentsAndNonPipes()
    {
        QProcess p;
        p.start(helperBinary(), {QStringLiteral("--anything")});
        QVERIFY(p.waitForFinished(5000));
        QCOMPARE(p.exitCode(), 2);

        QProcess file;
        file.setStandardInputFile(m_entry);  // a regular file, not a pipe
        file.start(helperBinary(), {});
        QVERIFY(file.waitForFinished(5000));
        QCOMPARE(file.exitCode(), 11);
    }

    void binaryHelloThenEofExits()
    {
        QProcess p;
        p.start(helperBinary(), {});
        QVERIFY(p.waitForStarted());
        QVERIFY(p.waitForReadyRead(5000));
        const QByteArray out = p.readAllStandardOutput();
        const Frame f = decodeOne(Bytes(out.begin(), out.end()));
        const auto h = decodeHello(f.payload);
        QVERIFY(h);
        QCOMPARE(h->euid, std::uint32_t(geteuid()));
        p.closeWriteChannel();  // EOF
        QVERIFY(p.waitForFinished(5000));
        QCOMPARE(p.exitStatus(), QProcess::NormalExit);
        QCOMPARE(p.exitCode(), 0);
        QVERIFY(p.readAllStandardError().contains("synbench-helper: started"));
    }

    void binaryReadSmbiosAsUserIsDenied()
    {
        if (geteuid() == 0)
            QSKIP("running as root");
        QProcess p;
        p.start(helperBinary(), {});
        QVERIFY(p.waitForStarted());
        const Bytes r = request(Op::ReadSmbios, 3);
        p.write(reinterpret_cast<const char *>(r.data()), qint64(r.size()));
        QByteArray out;
        Decoder d(kMaxResponseFrame);
        Frame f;
        int frames = 0;
        while (frames < 2 && p.waitForReadyRead(5000)) {
            out = p.readAllStandardOutput();
            d.feed(reinterpret_cast<const std::uint8_t *>(out.constData()), size_t(out.size()));
            while (d.next(f) == Decoder::Status::Frame)
                ++frames;
        }
        QCOMPARE(frames, 2);  // HELLO, then the answer
        QCOMPARE(f.requestId, 3u);
        QVERIFY(f.isError());
        QCOMPARE(decodeError(f.payload)->code, ErrorCode::NotPermitted);  // EACCES on the DMI table
        p.closeWriteChannel();
        QVERIFY(p.waitForFinished(5000));
    }

    void binaryExitsAfterThreeBadFrames()
    {
        QProcess p;
        p.start(helperBinary(), {});
        QVERIFY(p.waitForStarted());
        for (int i = 0; i < 3; ++i) {
            const Bytes r = request(Op(200 + i), std::uint32_t(i + 1));
            p.write(reinterpret_cast<const char *>(r.data()), qint64(r.size()));
        }
        QVERIFY(p.waitForFinished(5000));
        QCOMPARE(p.exitCode(), helper::Core::kExitTooManyBadFrames);
    }
};

QTEST_GUILESS_MAIN(TestHelperCore)
#include "tst_helper_core.moc"
