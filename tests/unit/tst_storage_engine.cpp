// SPDX-License-Identifier: GPL-3.0-or-later
// The Storage test's I/O engine (Phase 4b design §15.1, §16): the sync
// engine against a QTemporaryDir file — a write batch of known sizes returns
// the right bytes/ops, cancel after 2 ops drains, read latencies are positive.
// The io_uring / libaio / overlapped engines are exercised in their own slots
// (QSKIP where the build didn't compile them) and on their own OS by CI / A-W.
#include "tests/storage/io/engine.h"

#include <QFile>
#include <QTest>
#include <QTemporaryDir>

#include <cstdint>
#include <vector>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

using namespace sb::storage;

namespace {

// A plain (buffered) fd for the fixture files. The unit tests exercise the
// engines' submit/reap loops, not O_DIRECT alignment (which tmpfs — the CI and
// local /tmp filesystem — does not support); openDirectFile is tested
// separately and tolerantly.
#if defined(_WIN32)
int openPlain(const QString &path, bool read, bool write)
{
    int flags = _O_BINARY;
    if (read && write)
        flags |= _O_RDWR | _O_CREAT | _O_TRUNC;
    else if (write)
        flags |= _O_WRONLY | _O_CREAT | _O_TRUNC;
    else
        flags |= _O_RDONLY;
    return ::_open(path.toLocal8Bit().constData(), flags, _S_IREAD | _S_IWRITE);
}
void closeFd(int fd) { ::_close(fd); }
#else
int openPlain(const QString &path, bool read, bool write)
{
    int flags = O_CLOEXEC;
    if (read && write)
        flags |= O_RDWR | O_CREAT | O_TRUNC;
    else if (write)
        flags |= O_WRONLY | O_CREAT | O_TRUNC;
    else
        flags |= O_RDONLY;
    return ::open(path.toLocal8Bit().constData(), flags, 0666);
}
void closeFd(int fd) { ::close(fd); }
#endif

constexpr std::size_t kBlock = 4096;
constexpr std::size_t kBlocks = 4;
const CancelFn kNeverCancel = [] { return false; };

// The shared battery for any engine: the design's three §16 checks (write
// counts, cancel-after-2, positive read latencies) plus a read-back that the
// bytes landed where they should.
void exerciseEngine(IStorageIoEngine &engine, const QString &path)
{
    // Write 4 x 4 KiB: ops == 4, bytes == 16384.
    {
        std::vector<char> wbuf(kBlocks * kBlock, char(0x5A));
        IoView views[kBlocks];
        for (std::size_t i = 0; i < kBlocks; ++i)
            views[i] = IoView{wbuf.data() + i * kBlock, kBlock, i * kBlock};

        const int fd = openPlain(path, /*read=*/true, /*write=*/true);
        QVERIFY(fd >= 0);
        IoStats st;
        QString err;
        QVERIFY2(engine.runBatch(views, kBlocks, IoOp::Write, fd, st, kNeverCancel, &err), qPrintable(err));
        QCOMPARE(st.ops, std::uint64_t(4));
        QCOMPARE(st.bytes, std::uint64_t(16384));
        QVERIFY(st.latenciesUs.empty());   // writes feed ops/bytes only
        closeFd(fd);
    }

    // Read the four blocks back: ops == 4, bytes land correctly, latencies > 0.
    {
        std::vector<char> rbuf(kBlocks * kBlock, char(0));
        IoView views[kBlocks];
        for (std::size_t i = 0; i < kBlocks; ++i)
            views[i] = IoView{rbuf.data() + i * kBlock, kBlock, i * kBlock};

        const int fd = openPlain(path, /*read=*/true, /*write=*/false);
        QVERIFY(fd >= 0);
        IoStats st;
        QString err;
        QVERIFY2(engine.runBatch(views, kBlocks, IoOp::Read, fd, st, kNeverCancel, &err), qPrintable(err));
        QCOMPARE(st.ops, std::uint64_t(4));
        QCOMPARE(st.bytes, std::uint64_t(16384));
        QCOMPARE(st.latenciesUs.size(), std::size_t(4));
        for (double us : st.latenciesUs)
            QVERIFY(us > 0.0);
        closeFd(fd);
        for (char c : rbuf)
            QCOMPARE(c, char(0x5A));   // the write phase's fill, read back intact
    }

    // Cancel after 2 ops: ops == 2 (in-flight work drains; nothing left behind).
    {
        std::vector<char> wbuf(kBlocks * kBlock, char(0x6B));
        IoView views[kBlocks];
        for (std::size_t i = 0; i < kBlocks; ++i)
            views[i] = IoView{wbuf.data() + i * kBlock, kBlock, i * kBlock};

        const int fd = openPlain(path, /*read=*/true, /*write=*/true);
        QVERIFY(fd >= 0);
        IoStats st;
        QString err;
        int polls = 0;
        auto cancel = [&polls]() { return ++polls > 2; };   // true on the 3rd poll
        QVERIFY2(engine.runBatch(views, kBlocks, IoOp::Write, fd, st, cancel, &err), qPrintable(err));
        QCOMPARE(st.ops, std::uint64_t(2));
        closeFd(fd);
    }
}

} // namespace

class TestStorageEngine : public QObject
{
    Q_OBJECT

private slots:
    void syncEngineBattery()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        std::unique_ptr<IStorageIoEngine> engine = makeSyncEngine();
        exerciseEngine(*engine, dir.filePath(QStringLiteral("file.dat")));
    }

    void uringEngineBattery()
    {
#if defined(SYNTHEON_HAVE_LIBURING)
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        std::unique_ptr<IStorageIoEngine> engine = makeUringEngine();
        exerciseEngine(*engine, dir.filePath(QStringLiteral("file.dat")));
#else
        QSKIP("io_uring engine not compiled on this platform");
#endif
    }

    void backendNameIsSync()
    {
        QCOMPARE(makeSyncEngine()->backendName(), QStringLiteral("sync"));
    }

    void directAlignmentRule()
    {
        // Pure rule (R2): 512 B -> 4 KiB, 4 KiB -> 4 KiB, 16 KiB honoured.
        QCOMPARE(alignFromBlock(512), std::size_t(4096));
        QCOMPARE(alignFromBlock(4096), std::size_t(4096));
        QCOMPARE(alignFromBlock(8192), std::size_t(8192));
        QCOMPARE(alignFromBlock(16384), std::size_t(16384));

        // The real filesystem: >= 4 KiB and a whole multiple of it (tmpfs ext4
        // btrfs xfs all report 4096; a 16 KiB-block NVMe namespace is honoured).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::size_t a = directAlignment(dir.path());
        QVERIFY(a >= 4096);
        QCOMPARE(a % 4096, std::size_t(0));
    }

    void openDirectFileReport()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString p = dir.filePath(QStringLiteral("direct.dat"));
        QString err;
        const std::optional<int> fd = openDirectFile(p, /*read=*/false, /*write=*/true, &err);
        if (!fd) {
            // The fixture fs may not support O_DIRECT (tmpfs, overlayfs): the
            // failure must carry a reason, never a silent buffered fallback.
            QVERIFY(!err.isEmpty());
            return;
        }
        QVERIFY(*fd >= 0);
        closeFd(*fd);
        QFile::remove(p);
    }
};

QTEST_GUILESS_MAIN(TestStorageEngine)
#include "tst_storage_engine.moc"
