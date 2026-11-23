// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Storage test's per-OS async I/O engines (Phase 4b design §4.1).
// One small interface with four concrete engines chosen at run time
// (pickEngine, §4.1 selection / D7):
//   sync        engine_sync.cpp            everywhere (ARM64, Q1T1, fallback)
//   io_uring    engine_linux_uring.cpp     Linux x64 + liburing (preferred)
//   libaio      engine_linux_aio.cpp        Linux + libaio (the D7 fallback)
//   overlapped  engine_win.cpp             Windows (FILE_FLAG_OVERLAPPED)
// The interface below is §4.1 verbatim; pickEngine / IoDepth / alignFromBlock
// and the factory declarations are the task 5–8 additions it needs.

#include <QByteArray>
#include <QString>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace sb::storage {

enum class IoOp { Read, Write };

// One submitted operation, completed out of order.
struct IoStats {
    std::uint64_t ops = 0;          // completed ops
    std::uint64_t bytes = 0;        // completed bytes (transferred)
    // Completion latency in microseconds, one per completed op (Read ops only
    // feed latencyPercentiles; Write ops only feed ops/bytes).
    std::vector<double> latenciesUs;  // Read ops only
};

using CancelFn = std::function<bool()>;       // poll >= every 50 ms; true => stop submitting

// A byte-slice of an aligned buffer, de-aliased from the ownership concerns:
// the engine never owns `data`; callers keep the arena alive for the batch.
struct IoView {
    void *data = nullptr;        // aligned (P7)
    std::uint64_t length = 0;    // bytes; aligned multiple
    std::uint64_t offset = 0;    // file offset, aligned
};

class IStorageIoEngine {
public:
    virtual ~IStorageIoEngine() = default;
    virtual QString backendName() const = 0;      // "io_uring", "libaio", "overlapped", "sync"
    // Submit the batch (views interleavable); completes all before returning.
    // Appends to `out`. Must return promptly once `cancel` is true; the ops
    // already in flight are drained. Returns false + error only on a real I/O
    // failure (ENOSPC, EIO, …), not on cancel.
    virtual bool runBatch(const IoView *views, std::size_t count, IoOp op,
                          int fd, IoStats &out, const CancelFn &cancel,
                          QString *error) = 0;
    // A batch may cap its in-flight count (never exceed `maxInFlight` total).
    virtual std::size_t maxInFlight() const = 0;
    // False when the backend couldn't initialise at run time (io_uring blocked
    // by seccomp / kernel.io_uring_disabled, libaio io_setup refused). pickEngine
    // falls back to the next engine rather than failing every queued batch.
    virtual bool usable() const { return true; }
};

// Open the aligned file in the right mode (O_DIRECT on Linux,
// FILE_FLAG_NO_BUFFERING on Windows); a writable, direct-capable fd. No
// write-through: writes land in the device's own write cache (fio --direct=1).
// Throws/returns error on failure.
std::optional<int> openDirectFile(const QString &path, bool read, bool write,
                                  QString *error);

// The alignment the file and buffers must satisfy (Linux: the fs block size
// via statfs/fsync path, floored to 4096; Windows: the sector size, 4096).
std::size_t directAlignment(const QString &path);   // bytes; >= 4096

// ----- additions for tasks 5–8 (not in §4.1's interface) -----

// The pure alignment rule (§4.2 P7, risk R2): the fs/device block size rounded
// up to a whole power-of-two multiple, floored at 4 KiB — a 512 B logical
// block -> 4 KiB, and a 16 KiB NVMe namespace -> 16 KiB. A power of two is
// what O_DIRECT / unbuffered I/O actually accept, so a non-power-of-two block
// (e.g. a 12 KiB fs block) must round up rather than pass through. Factored
// out so the unit test can cover the 16 KiB case without a real 16 KiB-block
// filesystem.
constexpr std::size_t alignFromBlock(std::size_t blockBytes)
{
    return std::bit_ceil(std::max<std::size_t>(blockBytes, 4096));
}

// The queue-depth class a pattern drives (§4.1): Q1T1 uses the sync engine on
// every OS (depth 1 is sequential by definition); Q8/Q32 use the async engine.
enum class IoDepth { One, Queued };

// Pick the engine for a pattern's depth (§4.1 selection, D7). Never returns
// nullptr. One -> sync everywhere; Queued -> overlapped on Windows, io_uring
// on Linux x64 when liburing was compiled, else libaio on Linux, else sync.
// `queueDepth` sizes the async engine's in-flight cap (8 for Q8, 32 for Q32).
std::unique_ptr<IStorageIoEngine> pickEngine(IoDepth depth, std::size_t queueDepth = 32);

// The first engine in `candidates` that reports usable(), else the sync
// engine. Factored out of pickEngine so a unit test can inject a failing one.
std::unique_ptr<IStorageIoEngine> firstUsableEngine(std::vector<std::unique_ptr<IStorageIoEngine>> candidates);

// The concrete engine constructors (one per engine TU). Only the sync engine
// is always built (depth 1); the async engines take their queue depth, so
// callers size the in-flight cap to the pattern (8 for Q8, 32 for Q32). Guard
// with SYNTHEON_HAVE_LIBURING / SYNTHEON_HAVE_LIBAIO / _WIN32.
std::unique_ptr<IStorageIoEngine> makeSyncEngine();
#if defined(SYNTHEON_HAVE_LIBURING)
std::unique_ptr<IStorageIoEngine> makeUringEngine(std::size_t queueDepth = 32);
#endif
#if defined(SYNTHEON_HAVE_LIBAIO)
std::unique_ptr<IStorageIoEngine> makeAioEngine(std::size_t queueDepth = 32);
#endif
#if defined(_WIN32)
std::unique_ptr<IStorageIoEngine> makeWinEngine(std::size_t queueDepth = 32);
#endif

} // namespace sb::storage
