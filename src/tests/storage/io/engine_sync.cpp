// SPDX-License-Identifier: GPL-3.0-or-later
// The sync engine (Phase 4b design §4.1): the ARM64 / Q1T1 / fallback path.
// A plain pread/pwrite loop, one op at a time, with the CancelFn polled before
// every op (finer than the 50 ms floor, P-STOP) and per-op read latency. Also
// the Linux openDirectFile/directAlignment helpers (the Windows ones are in
// engine_win.cpp).
#include "tests/storage/io/engine.h"

#include <QFileInfo>

#include <chrono>

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
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

// One positioned read/write. POSIX: pread(2)/pwrite(2). Windows: the fd is a
// FILE_FLAG_OVERLAPPED handle (openDirectFile), so the CRT _read/_write would
// fail with EINVAL; drive a positioned ReadFile/WriteFile on the recovered
// HANDLE with a local OVERLAPPED + manual-reset event instead. The sync engine
// is single-threaded (maxInFlight() == 1), so blocking on the one result is
// what a "sync" backend means.
std::int64_t readAt(int fd, void *buf, std::uint64_t len, std::uint64_t off, void *evt, unsigned long *osErr)
{
#ifdef _WIN32
    const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        *osErr = ERROR_INVALID_HANDLE;
        return -1;
    }
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    // One manual-reset event per engine (created once, outside the timed
    // region); reset before each op.
    ov.hEvent = static_cast<HANDLE>(evt);
    if (!ov.hEvent) {
        *osErr = ERROR_INVALID_HANDLE;
        return -1;
    }
    ResetEvent(ov.hEvent);
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, static_cast<DWORD>(len), &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(h, &ov, &n, TRUE);   // wait for completion
    if (!ok) {
        *osErr = GetLastError();
        return -1;
    }
    return static_cast<std::int64_t>(n);
#else
    (void)evt;
    (void)osErr;
    for (;;) {
        const ssize_t r = ::pread(fd, buf, len, static_cast<off_t>(off));
        if (r >= 0 || errno != EINTR)
            return r;
    }
#endif
}

std::int64_t writeAt(int fd, const void *buf, std::uint64_t len, std::uint64_t off, void *evt, unsigned long *osErr)
{
#ifdef _WIN32
    const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        *osErr = ERROR_INVALID_HANDLE;
        return -1;
    }
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    // One manual-reset event per engine (created once, outside the timed
    // region); reset before each op.
    ov.hEvent = static_cast<HANDLE>(evt);
    if (!ov.hEvent) {
        *osErr = ERROR_INVALID_HANDLE;
        return -1;
    }
    ResetEvent(ov.hEvent);
    DWORD n = 0;
    BOOL ok = WriteFile(h, buf, static_cast<DWORD>(len), &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(h, &ov, &n, TRUE);   // wait for completion
    if (!ok) {
        *osErr = GetLastError();
        return -1;
    }
    return static_cast<std::int64_t>(n);
#else
    (void)evt;
    (void)osErr;
    for (;;) {
        const ssize_t r = ::pwrite(fd, buf, len, static_cast<off_t>(off));
        if (r >= 0 || errno != EINTR)
            return r;
    }
#endif
}

#ifdef _WIN32
// GetLastError() -> "text (code N)" via FormatMessageW; errno is not set by
// the Win32 file API.
QString winErrorText(DWORD code)
{
    wchar_t *buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                       | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString text = n ? QString::fromWCharArray(buf, int(n)).trimmed() : QString();
    if (buf)
        LocalFree(buf);
    return text.isEmpty() ? QStringLiteral("error %1").arg(code)
                          : QStringLiteral("%1 (error %2)").arg(text).arg(code);
}
#endif

class SyncEngine final : public IStorageIoEngine
{
public:
#ifdef _WIN32
    // One event for every op this engine does, so CreateEventW/CloseHandle
    // stay out of the timed per-op region.
    SyncEngine() : m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~SyncEngine() override
    {
        if (m_event)
            CloseHandle(m_event);
    }
#endif

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
            unsigned long osErr = 0;
            const std::int64_t n = isRead ? readAt(fd, v.data, v.length, v.offset, eventHandle(), &osErr)
                                          : writeAt(fd, v.data, v.length, v.offset, eventHandle(), &osErr);
            const auto t1 = std::chrono::steady_clock::now();
            if (n < 0) {
                if (error)
                    *error = QStringLiteral("sync I/O failed: %1")
                                 .arg(osErrorText(osErr));
                return false;   // a real I/O failure, not cancel
            }
            if (static_cast<std::uint64_t>(n) != v.length) {
                if (error)
                    *error = QStringLiteral("sync short I/O: %1 of %2 bytes")
                                 .arg(n).arg(v.length);
                return false;   // EOF / short transfer: never count a partial op
            }
            out.ops += 1;
            out.bytes += static_cast<std::uint64_t>(n);
            if (isRead)
                out.latenciesUs.push_back(
                    std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        return true;
    }

private:
#ifdef _WIN32
    void *eventHandle() const { return m_event; }
    static QString osErrorText(unsigned long code) { return winErrorText(static_cast<DWORD>(code)); }
    HANDLE m_event = nullptr;
#else
    static void *eventHandle() { return nullptr; }
    static QString osErrorText(unsigned long) { return QString::fromLatin1(std::strerror(errno)); }
#endif
};

} // namespace

std::unique_ptr<IStorageIoEngine> makeSyncEngine()
{
    return std::make_unique<SyncEngine>();
}

#if !defined(_WIN32)
std::size_t directAlignment(const QString &path)
{
    // statfs on the nearest EXISTING ancestor. The path may be a directory
    // (a mount point — use it directly) or a file that does not exist yet
    // (§7 sizes the arena before the file is created, and the target mount
    // folder may not exist yet either). Keep stripping the last component
    // until a directory that exists is reached, then stat that; f_bsize is
    // raised to a power-of-two 4 KiB floor by alignFromBlock (§4.2 P7, R2).
    QString dir = QFileInfo(path).isDir() ? QFileInfo(path).absoluteFilePath()
                                          : QFileInfo(path).absolutePath();
    for (;;) {
        struct statfs fs{};
        if (::statfs(dir.toLocal8Bit().constData(), &fs) == 0 && fs.f_bsize > 0)
            return alignFromBlock(std::size_t(fs.f_bsize));
        const QString parent = QFileInfo(dir).absolutePath();
        if (parent == dir)
            break;   // the filesystem root, with nothing above it
        dir = parent;
    }
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
    // O_DIRECT only, no O_SYNC: writes land in the device's own write cache,
    // matching fio --direct=1 and CrystalDiskMark (write-through would depress
    // write/mixed throughput 10-200x).

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
