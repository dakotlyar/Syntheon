// SPDX-License-Identifier: GPL-3.0-or-later
// The Windows overlapped engine (Phase 4b design §4.1, P5): one OVERLAPPED +
// manual-reset event per queue slot (handed a HANDLE passed as an int fd via
// _open_osfhandle), a bounded window (never more than maxInFlight in flight),
// completion via WaitForMultipleObjects on the overlapped events (no IOCP —
// one thread, depth <= 32), the CancelFn polled on each 50 ms timeout, and
// per-op read latency. Also the Windows openDirectFile/directAlignment helpers.
#include "tests/storage/io/engine.h"

#include <QDir>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace sb::storage {

namespace {

constexpr std::size_t kDepth = 32;   // the max pattern queue depth (Q32)

class WinEngine final : public IStorageIoEngine
{
public:
    explicit WinEngine(std::size_t depth) : m_depth(depth), m_slots(depth), m_ready(true)
    {
        for (auto &s : m_slots) {
            s.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual-reset
            if (!s.event)
                m_ready = false;
        }
    }

    ~WinEngine() override
    {
        for (auto &s : m_slots)
            if (s.event)
                CloseHandle(s.event);
    }

    QString backendName() const override { return QStringLiteral("overlapped"); }

    std::size_t maxInFlight() const override { return m_depth; }

    bool runBatch(const IoView *views, std::size_t count, IoOp op,
                  int fd, IoStats &out, const CancelFn &cancel,
                  QString *error) override
    {
        if (!m_ready) {
            if (error)
                *error = QStringLiteral("overlapped: CreateEventW failed");
            return false;
        }
        const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (h == INVALID_HANDLE_VALUE || h == nullptr) {
            if (error)
                *error = QStringLiteral("overlapped: invalid handle");
            return false;
        }

        const bool isRead = (op == IoOp::Read);
        std::size_t submitted = 0;   // views handed to the file so far
        std::size_t completed = 0;   // results collected so far
        QString why;
        bool cancelled = false;
        // Latch: once cancel() reports true, stop polling it for the rest of
        // the batch (it may be expensive or one-shot).
        auto stop = [&] {
            if (!cancelled && cancel())
                cancelled = true;
            return cancelled;
        };

        auto now = [] { return std::chrono::steady_clock::now(); };

        // Start overlapped ops into free slots until the in-flight cap or
        // cancel stops us. An op that completes synchronously (ReadFile
        // returns TRUE) is recorded immediately — no event will signal it.
        auto submitWindow = [&] {
            if (!why.isEmpty())
                return;
            while (submitted < count && submitted - completed < m_depth) {
                if (stop())
                    break;
                std::size_t slot = std::size_t(-1);
                for (std::size_t s = 0; s < m_depth; ++s) {
                    if (!m_slots[s].pending) {
                        slot = s;
                        break;
                    }
                }
                if (slot == std::size_t(-1))
                    break;
                Slot &sl = m_slots[slot];
                const IoView &v = views[submitted];
                std::memset(&sl.ov, 0, sizeof sl.ov);
                sl.ov.Offset = static_cast<DWORD>(v.offset & 0xFFFFFFFFu);
                sl.ov.OffsetHigh = static_cast<DWORD>(v.offset >> 32);
                sl.ov.hEvent = sl.event;
                sl.t0 = now();

                DWORD immediate = 0;
                const BOOL ok = isRead ? ReadFile(h, v.data, static_cast<DWORD>(v.length), &immediate, &sl.ov)
                                       : WriteFile(h, v.data, static_cast<DWORD>(v.length), &immediate, &sl.ov);
                if (ok) {
                    if (static_cast<std::uint64_t>(immediate) != v.length) {
                        why = QStringLiteral("overlapped short op: %1 of %2 bytes")
                                  .arg(immediate).arg(v.length);
                        CancelIoEx(h, nullptr);
                        break;
                    }
                    // Completed synchronously: count it now, the slot stays free;
                    // both submitted and completed advance by one.
                    out.ops += 1;
                    out.bytes += static_cast<std::uint64_t>(immediate);
                    if (isRead)
                        out.latenciesUs.push_back(
                            std::chrono::duration<double, std::micro>(now() - sl.t0).count());
                    ++completed;
                    ++submitted;
                    continue;
                }
                if (GetLastError() != ERROR_IO_PENDING) {
                    // A real submit failure: the slot never went pending. Stop
                    // submitting, cancel what is already in flight, and drain it
                    // below before returning.
                    why = QStringLiteral("overlapped submit failed: error %1").arg(GetLastError());
                    CancelIoEx(h, nullptr);
                    break;
                }
                sl.pending = true;
                sl.length = v.length;
                ++submitted;
            }
        };

        submitWindow();
        std::vector<HANDLE> waitFor(m_depth);
        std::vector<std::size_t> slotOf(m_depth);
        while (why.isEmpty() && completed < submitted) {
            std::size_t npending = 0;
            for (std::size_t s = 0; s < m_depth; ++s) {
                if (m_slots[s].pending) {
                    waitFor[npending] = m_slots[s].event;
                    slotOf[npending] = s;
                    ++npending;
                }
            }
            if (npending == 0)
                break;
            const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(npending), waitFor.data(), FALSE, 50);
            if (r == WAIT_TIMEOUT) {
                submitWindow();   // poll cancel and top the batch back up
                continue;
            }
            if (r == WAIT_FAILED) {
                why = QStringLiteral("WaitForMultipleObjects failed: error %1").arg(GetLastError());
                break;
            }
            // WaitForMultipleObjects returns the lowest signalled index; sweep
            // from there to the end and reap every signalled slot, so a low slot
            // never starves a higher one at Q32.
            const std::size_t first = static_cast<std::size_t>(r - WAIT_OBJECT_0);
            for (std::size_t i = first; i < npending; ++i) {
                if (WaitForSingleObject(waitFor[i], 0) != WAIT_OBJECT_0)
                    continue;   // not (yet) signalled
                Slot &sl = m_slots[slotOf[i]];
                DWORD transferred = 0;
                if (!GetOverlappedResult(h, &sl.ov, &transferred, FALSE)) {
                    why = QStringLiteral("GetOverlappedResult failed: error %1").arg(GetLastError());
                    break;
                }
                ResetEvent(sl.event);
                sl.pending = false;
                if (static_cast<std::uint64_t>(transferred) != sl.length) {
                    why = QStringLiteral("overlapped short op: %1 of %2 bytes")
                              .arg(transferred).arg(sl.length);
                    break;
                }
                out.ops += 1;
                out.bytes += static_cast<std::uint64_t>(transferred);
                if (isRead)
                    out.latenciesUs.push_back(
                        std::chrono::duration<double, std::micro>(now() - sl.t0).count());
                ++completed;
            }
            submitWindow();
        }

        if (!why.isEmpty()) {
            // Drain every outstanding op (cancelled ones included) so no kernel
            // op is still DMA-ing into the caller's buffers when we return.
            for (auto &sl : m_slots) {
                if (sl.pending) {
                    DWORD transferred = 0;
                    GetOverlappedResult(h, &sl.ov, &transferred, TRUE);   // block
                    ResetEvent(sl.event);
                    sl.pending = false;
                }
            }
            if (error)
                *error = why;
            return false;
        }
        return true;
    }

private:
    struct Slot {
        OVERLAPPED ov{};
        HANDLE event = nullptr;
        bool pending = false;
        std::uint64_t length = 0;   // expected transfer (short-op check)
        std::chrono::steady_clock::time_point t0{};
    };

    std::size_t m_depth = 0;
    std::vector<Slot> m_slots;
    bool m_ready = false;
};

} // namespace

std::unique_ptr<IStorageIoEngine> makeWinEngine()
{
    return std::make_unique<WinEngine>(kDepth);
}

std::size_t directAlignment(const QString &path)
{
    // GetVolumePathNameW resolves a file or directory to its volume root
    // ("C:\"), which GetDiskFreeSpaceW wants; its bytes-per-sector is raised
    // to a power-of-two 4 KiB floor by alignFromBlock (§4.2 P7).
    const std::wstring full = QDir::toNativeSeparators(path).toStdWString();
    wchar_t volume[MAX_PATH + 1] = L"";
    if (GetVolumePathNameW(full.c_str(), volume, MAX_PATH + 1)) {
        DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, totalClusters = 0;
        if (GetDiskFreeSpaceW(volume, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters)
            && bytesPerSector > 0)
            return alignFromBlock(std::size_t(bytesPerSector));
    }
    return 4096;
}

std::optional<int> openDirectFile(const QString &path, bool read, bool write, QString *error)
{
    DWORD access = 0;
    if (read)
        access |= GENERIC_READ;
    if (write)
        access |= GENERIC_WRITE;
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    // OPEN_ALWAYS, not CREATE_ALWAYS: the file is prepared (filled) by an
    // earlier open; a re-open for the measured phases must not truncate it.
    const DWORD create = write ? OPEN_ALWAYS : OPEN_EXISTING;
    // Cache bypass (P7, §26): unbuffered + write-through overlapped.
    const DWORD flags = FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;

    const std::wstring w = path.toStdWString();
    HANDLE h = CreateFileW(w.c_str(), access, share, nullptr, create, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error)
            *error = QStringLiteral("openDirectFile: CreateFileW failed: error %1").arg(GetLastError());
        return std::nullopt;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h),
                                   _O_BINARY | (read && write ? _O_RDWR : write ? _O_WRONLY : _O_RDONLY));
    if (fd < 0) {
        CloseHandle(h);
        if (error)
            *error = QStringLiteral("openDirectFile: _open_osfhandle failed: %1").arg(errno);
        return std::nullopt;
    }
    return fd;
}

} // namespace sb::storage
