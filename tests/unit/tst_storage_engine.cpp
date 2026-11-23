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
#include <cstring>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#  include <string>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

using namespace sb::storage;

namespace {

// An int fd for the fixture files. The unit tests exercise the engines'
// submit/reap loops, not O_DIRECT alignment, so the fd is buffered; on Windows
// it is still FILE_FLAG_OVERLAPPED, the shape openDirectFile produces (the
// sync and overlapped engines recover the HANDLE and do positioned I/O).
#if defined(_WIN32)
int openPlain(const QString &path, bool read, bool write)
{
    DWORD access = 0;
    if (read)
        access |= GENERIC_READ;
    if (write)
        access |= GENERIC_WRITE;
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD create = write ? CREATE_ALWAYS : OPEN_EXISTING;
    const std::wstring w = path.toStdWString();
    HANDLE h = CreateFileW(w.c_str(), access, share, nullptr, create, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_BINARY | _O_RDWR);
    if (fd < 0)
        CloseHandle(h);
    return fd;
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

    // A batch larger than the async in-flight cap (64 > kDepth 32): the engine
    // must window the submissions (multiple submit/reap rounds) and reuse the
    // free slots until every op is done.
    {
        constexpr std::size_t kDeep = 64;
        std::vector<char> wbuf(kDeep * kBlock, char(0x3C));
        std::vector<IoView> views(kDeep);
        for (std::size_t i = 0; i < kDeep; ++i)
            views[i] = IoView{wbuf.data() + i * kBlock, kBlock, i * kBlock};

        const int fd = openPlain(path, /*read=*/true, /*write=*/true);
        QVERIFY(fd >= 0);
        IoStats st;
        QString err;
        QVERIFY2(engine.runBatch(views.data(), kDeep, IoOp::Write, fd, st, kNeverCancel, &err), qPrintable(err));
        QCOMPARE(st.ops, std::uint64_t(kDeep));
        QCOMPARE(st.bytes, std::uint64_t(kDeep * kBlock));
        closeFd(fd);
    }
}

// An engine whose backend failed to initialise (io_uring blocked by seccomp).
class DeadEngine final : public IStorageIoEngine
{
public:
    QString backendName() const override { return QStringLiteral("dead"); }
    bool runBatch(const IoView *, std::size_t, IoOp, int, IoStats &, const CancelFn &, QString *) override
    {
        return false;
    }
    std::size_t maxInFlight() const override { return 1; }
    bool usable() const override { return false; }
};

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

    void aioEngineBattery()
    {
#if defined(SYNTHEON_HAVE_LIBAIO)
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        std::unique_ptr<IStorageIoEngine> engine = makeAioEngine();
        exerciseEngine(*engine, dir.filePath(QStringLiteral("file.dat")));
#else
        QSKIP("libaio engine not compiled on this platform");
#endif
    }

    void winEngineBattery()
    {
#if defined(_WIN32)
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        std::unique_ptr<IStorageIoEngine> engine = makeWinEngine();
        QVERIFY(engine != nullptr);
        QCOMPARE(engine->backendName(), QStringLiteral("overlapped"));
        // openPlain on Windows opens FILE_FLAG_OVERLAPPED, so the engine's
        // _get_osfhandle path drives real overlapped ReadFile/WriteFile.
        exerciseEngine(*engine, dir.filePath(QStringLiteral("file.dat")));
#else
        QSKIP("overlapped engine only on Windows");
#endif
    }

    void backendNameIsSync()
    {
        QCOMPARE(makeSyncEngine()->backendName(), QStringLiteral("sync"));
    }

    void pickEngineSelection()
    {
        // Depth 1 is always the sync engine (P4); the queued depths pick the
        // platform's async backend (§4.1 selection, D7).
        QCOMPARE(pickEngine(IoDepth::One)->backendName(), QStringLiteral("sync"));

        std::unique_ptr<IStorageIoEngine> queued = pickEngine(IoDepth::Queued);
        QVERIFY(queued != nullptr);
#if defined(_WIN32)
        QCOMPARE(queued->backendName(), QStringLiteral("overlapped"));
#elif defined(SYNTHEON_HAVE_LIBURING)
        QCOMPARE(queued->backendName(), QStringLiteral("io_uring"));
#elif defined(SYNTHEON_HAVE_LIBAIO)
        QCOMPARE(queued->backendName(), QStringLiteral("libaio"));
#else
        QCOMPARE(queued->backendName(), QStringLiteral("sync"));
#endif
    }

    void unusableEngineFallsBack()
    {
        std::vector<std::unique_ptr<IStorageIoEngine>> c;
        c.push_back(std::make_unique<DeadEngine>());
        c.push_back(std::make_unique<DeadEngine>());
        QCOMPARE(firstUsableEngine(std::move(c))->backendName(), QStringLiteral("sync"));
        // And whatever pickEngine returns must be usable.
        QVERIFY(pickEngine(IoDepth::Queued)->usable());
    }

    void directAlignmentRule()
    {
        // Pure rule (R2): 512 B -> 4 KiB, 4 KiB -> 4 KiB, 16 KiB honoured, and
        // a non-power-of-two block rounds up to the next power of two (what
        // O_DIRECT actually accepts).
        QCOMPARE(alignFromBlock(512), std::size_t(4096));
        QCOMPARE(alignFromBlock(4096), std::size_t(4096));
        QCOMPARE(alignFromBlock(8192), std::size_t(8192));
        QCOMPARE(alignFromBlock(12288), std::size_t(16384));
        QCOMPARE(alignFromBlock(16384), std::size_t(16384));

        // The real filesystem: >= 4 KiB and a whole multiple of it (tmpfs ext4
        // btrfs xfs all report 4096; a 16 KiB-block NVMe namespace is honoured).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::size_t a = directAlignment(dir.path());
        QVERIFY(a >= 4096);
        QCOMPARE(a % 4096, std::size_t(0));

        // The nearest-existing-ancestor walk: a path that doesn't exist yet (a
        // not-yet-created target folder + file, §7 pre-creation sizing)
        // resolves to the same filesystem as the existing ancestor.
        const QString deep = dir.filePath(QStringLiteral("not-there/sub/file.dat"));
        QCOMPARE(directAlignment(deep), a);
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

    // A real write+read through the O_DIRECT fd (skipped where the fixture
    // filesystem does not support direct I/O, e.g. tmpfs).
    void openDirectFileRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString p = dir.filePath(QStringLiteral("direct.dat"));
        QString err;
        const std::optional<int> fd = openDirectFile(p, /*read=*/true, /*write=*/true, &err);
        if (!fd)
            QSKIP("filesystem does not support O_DIRECT (openDirectFile failed)");
        QVERIFY(*fd >= 0);

        // An aligned 4 KiB buffer (O_DIRECT requires sector-aligned length,
        // offset and address).
        std::vector<char> storage(kBlock * 2 + 4095);
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(storage.data());
        void *aligned = reinterpret_cast<void *>((base + 4095) & ~std::uintptr_t(4095));
        auto *buf = static_cast<char *>(aligned);

        const std::unique_ptr<IStorageIoEngine> engine = makeSyncEngine();

        std::memset(buf, char(0x42), kBlock);
        IoView wv{buf, kBlock, 0};
        IoStats wst;
        QVERIFY2(engine->runBatch(&wv, 1, IoOp::Write, *fd, wst, kNeverCancel, &err), qPrintable(err));
        QCOMPARE(wst.ops, std::uint64_t(1));
        QCOMPARE(wst.bytes, std::uint64_t(kBlock));

        std::memset(buf, char(0), kBlock);
        IoView rv{buf, kBlock, 0};
        IoStats rst;
        QVERIFY2(engine->runBatch(&rv, 1, IoOp::Read, *fd, rst, kNeverCancel, &err), qPrintable(err));
        QCOMPARE(rst.ops, std::uint64_t(1));
        QCOMPARE(rst.bytes, std::uint64_t(kBlock));
        QCOMPARE(rst.latenciesUs.size(), std::size_t(1));

        for (std::size_t i = 0; i < kBlock; ++i)
            QCOMPARE(buf[i], char(0x42));   // the written pattern, read back intact

        closeFd(*fd);
        QFile::remove(p);
    }
};

QTEST_GUILESS_MAIN(TestStorageEngine)
#include "tst_storage_engine.moc"
