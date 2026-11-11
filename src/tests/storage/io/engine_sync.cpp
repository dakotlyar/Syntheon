// SPDX-License-Identifier: GPL-3.0-or-later
// The sync engine (Phase 4b design §4.1): the ARM64 / Q1T1 / fallback path.
// A plain pread/pwrite loop, one op at a time, with the CancelFn polled before
// every op (finer than the 50 ms floor, P-STOP) and per-op read latency. Also
// the Linux openDirectFile/directAlignment helpers (the Windows ones are in
// engine_win.cpp).
#include "tests/storage/io/engine.h"

#include <chrono>

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#else
#  include <fcntl.h>
#  include <sys/statfs.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace sb::storage {

namespace {

// One positioned read/write. POSIX: pread(2)/pwrite(2). Windows: the CRT has
// no pread/pwrite, so seek + read/write — the sync engine is single-threaded
// (Q1T1; maxInFlight() == 1), so the shared file position is safe.
std::int64_t readAt(int fd, void *buf, std::uint64_t len, std::uint64_t off)
{
#ifdef _WIN32
    if (_lseeki64(fd, std::int64_t(off), SEEK_SET) < 0)
        return -1;
    return _read(fd, buf, static_cast<unsigned>(len));
#else
    return ::pread(fd, buf, len, static_cast<off_t>(off));
#endif
}

std::int64_t writeAt(int fd, const void *buf, std::uint64_t len, std::uint64_t off)
{
#ifdef _WIN32
    if (_lseeki64(fd, std::int64_t(off), SEEK_SET) < 0)
        return -1;
    return _write(fd, buf, static_cast<unsigned>(len));
#else
    return ::pwrite(fd, buf, len, static_cast<off_t>(off));
#endif
}

class SyncEngine final : public IStorageIoEngine
{
public:
    QString backendName() const override { return QStringLiteral("sync"); }

    std::size_t maxInFlight() const override { return 1; }

    bool runBatch(const IoView *views, std::size_t count, IoOp op,
                  int fd, IoStats &out, const CancelFn &cancel,
                  QString *error) override
    {
        const bool isRead = (op == IoOp::Read);
        for (std::size_t i = 0; i < count; ++i) {
            if (cancel())
                break;   // stop submitting; a sync op is never left in flight
            const IoView &v = views[i];
            const auto t0 = std::chrono::steady_clock::now();
            const std::int64_t n = isRead ? readAt(fd, v.data, v.length, v.offset)
                                          : writeAt(fd, v.data, v.length, v.offset);
            const auto t1 = std::chrono::steady_clock::now();
            if (n < 0) {
                if (error)
                    *error = QStringLiteral("sync I/O failed: %1")
                                 .arg(QString::fromLatin1(std::strerror(errno)));
                return false;   // a real I/O failure, not cancel
            }
            out.ops += 1;
            out.bytes += static_cast<std::uint64_t>(n);
            if (isRead)
                out.latenciesUs.push_back(
                    std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        return true;
    }
};

} // namespace

std::unique_ptr<IStorageIoEngine> makeSyncEngine()
{
    return std::make_unique<SyncEngine>();
}

#if !defined(_WIN32)
std::size_t directAlignment(const QString &path)
{
    // The filesystem's optimal block size (statfs f_bsize), rounded up to a
    // 4 KiB multiple (§4.2 P7): > 4096 on a 16 KiB-block NVMe namespace (R2).
    struct statfs fs{};
    if (::statfs(path.toLocal8Bit().constData(), &fs) == 0 && fs.f_bsize > 0)
        return alignFromBlock(std::size_t(fs.f_bsize));
    return 4096;
}

std::optional<int> openDirectFile(const QString &path, bool read, bool write, QString *error)
{
    int flags = O_DIRECT;
    if (read && write)
        flags |= O_RDWR | O_CREAT;
    else if (write)
        flags |= O_WRONLY | O_CREAT;
    else
        flags |= O_RDONLY;
    // Cache bypass (P7, §26): writes go through to the device (O_SYNC); the
    // signature's `write` carries the §4.1 "writeThrough" intent.
    if (write)
        flags |= O_SYNC;

    const int fd = ::open(path.toLocal8Bit().constData(), flags, 0666);
    if (fd < 0) {
        if (error)
            *error = QStringLiteral("openDirectFile: cannot open %1 (O_DIRECT): %2")
                         .arg(path, QString::fromLatin1(std::strerror(errno)));
        return std::nullopt;
    }
    return fd;
}
#endif

} // namespace sb::storage
