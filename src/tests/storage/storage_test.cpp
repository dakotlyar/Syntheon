// SPDX-License-Identifier: GPL-3.0-or-later
// The Storage test (Phase 4b design): info/probe/options/preconditions/
// validate/estimate/timeLimit and score; the run loop is §8 (task 11) and the
// Detail page is storage_detail.cpp. The drive picker reads the snapshot's
// `storage` array (§3.1, N1), falling back to QStorageInfo::mountedVolumes();
// the free-space guard, blocklist and wear note live in req.{h,cpp} (§6).
#include "tests/storage/storage_test.h"

#include "core/fs_util.h"
#include "tests/cpu/kernels/prng.h"
#include "tests/storage/formula_v1.h"
#include "tests/storage/ids.h"
#include "tests/storage/io/engine.h"
#include "tests/storage/req.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <optional>
#include <vector>

namespace sb::tests {

namespace {

// One candidate target drive, from the snapshot or the QStorageInfo fallback.
struct Drive {
    QString mount;      // canonical root path
    QString fileSystem;
    qint64 freeBytes = 0;
    qint64 totalBytes = 0;
    QString model;      // the drive behind it
    QString bus;        // "NVMe", "SATA", …
    QString kind;       // "SSD", "HDD", ""
};

// The snapshot's `storage` array, else the live QStorageInfo volumes (P9: the
// field predates some snapshots). Both already exclude non-/dev/ and loop
// devices; the fallback re-applies the stub's isValid/isReady/readOnly and
// ≥ 1 GiB filters.
QVector<Drive> listDrives(const SystemSnapshot &snapshot)
{
    QVector<Drive> out;
    const QJsonArray vols = snapshot.data.value(QStringLiteral("storage")).toArray();
    if (!vols.isEmpty()) {
        for (const QJsonValue &v : vols) {
            const QJsonObject o = v.toObject();
            Drive d;
            d.mount = o.value(QStringLiteral("mount")).toString();
            d.fileSystem = o.value(QStringLiteral("file_system")).toString();
            d.freeBytes = qint64(o.value(QStringLiteral("free_bytes")).toDouble());
            d.totalBytes = qint64(o.value(QStringLiteral("total_bytes")).toDouble());
            d.model = o.value(QStringLiteral("model")).toString();
            d.bus = o.value(QStringLiteral("bus")).toString();
            d.kind = o.value(QStringLiteral("kind")).toString();
            if (!d.mount.isEmpty())
                out.append(d);
        }
        return out;
    }
    for (const QStorageInfo &v : QStorageInfo::mountedVolumes()) {
        if (!v.isValid() || !v.isReady() || v.isReadOnly() || v.bytesTotal() < (qint64(1) << 30))
            continue;
#ifdef Q_OS_UNIX
        const QString device = QString::fromLocal8Bit(v.device());
        if (!device.startsWith(QLatin1StringView("/dev/")) || device.startsWith(QLatin1StringView("/dev/loop")))
            continue;
#endif
        Drive d;
        d.mount = v.rootPath();
        d.fileSystem = QString::fromLatin1(v.fileSystemType());
        d.freeBytes = v.bytesAvailable();
        d.totalBytes = v.bytesTotal();
        out.append(d);
    }
    return out;
}

// The file-size option value ("1g"/"4g"/"16g"/"64g") to bytes (GiB).
qint64 fileSizeBytes(const QString &key)
{
    const qint64 gib = qint64(1) << 30;
    if (key == QLatin1StringView("1g"))
        return gib;
    if (key == QLatin1StringView("4g"))
        return 4 * gib;
    if (key == QLatin1StringView("16g"))
        return 16 * gib;
    if (key == QLatin1StringView("64g"))
        return 64 * gib;
    return 4 * gib;   // the default
}

// The free-space refusal string for a drive with `freeBytes` free, or empty
// when there is room (§3.3; the run-time value wins, so both validate() and
// run() share this).
QString freeSpaceError(qint64 freeBytes, qint64 size)
{
    return sb::storage::hasEnoughFreeSpace(freeBytes, size) ? QString()
                                                            : sb::storage::freeSpaceRefusal(freeBytes, size);
}

// A writable directory on the SAME block device as `mount` (§3.2 D5: the test
// must benchmark `mount`, but the mount root may not be user-writable, e.g.
// "/" on a CI runner). Candidates: the mount, GenericCacheLocation, home,
// /var/tmp — the first writable one on the same device (never cross devices,
// never a tmpfs that isn't the mount itself).
QString writableParentOnDevice(const QString &mount)
{
    const QString device = QStorageInfo(mount).device();
    QStringList candidates;
    candidates << mount;
    candidates << QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    candidates << QDir::homePath();
    candidates << QStringLiteral("/var/tmp");
    for (const QString &candidate : candidates) {
        if (candidate.isEmpty())
            continue;
        if (QStorageInfo(candidate).device() != device)
            continue;
        const QFileInfo fi(candidate);
        if (fi.isDir() && fi.isWritable())
            return candidate;
    }
    return {};
}

// §3.5: a network mount is blocked by its filesystem/root (isNetworkMount), and
// on Windows also by GetDriveTypeW — a mapped drive reports "NTFS" but is
// DRIVE_REMOTE.
bool isBlockedDrive(const Drive &d)
{
    if (sb::storage::isNetworkMount(d.mount, d.fileSystem))
        return true;
#ifdef _WIN32
    const UINT type = GetDriveTypeW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(d.mount).utf16()));
    return sb::storage::isNetworkDriveType(std::uint32_t(type));
#else
    return false;
#endif
}

// D2: removable / optical drives are warned, not blocked.
bool isRemovableDrive(const Drive &d)
{
#ifdef _WIN32
    const UINT type = GetDriveTypeW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(d.mount).utf16()));
    return type == DRIVE_REMOVABLE || type == DRIVE_CDROM;
#else
    return d.bus == QLatin1StringView("USB");
#endif
}

} // namespace

// ---- the run loop (task 11, §8) ----
namespace {

constexpr std::uint64_t kSeed = 0x574B524C;                 // "WKRL" (§4.3, N4), the file content
constexpr std::uint64_t kOffsetSeed = 0x9E3779B97F4A7C15ull;  // the offset stream (distinct from the fill)
constexpr std::size_t kMaxDepth = 32;
constexpr std::size_t kSeqBlock = std::size_t(1) << 20;      // 1 MiB
constexpr std::size_t kRndBlock = 4096;                      // 4 KiB
constexpr int kRuns = 3;                                     // median of 3 (§2.1)
constexpr std::chrono::milliseconds kWindow{5000};           // 5 s per window
constexpr std::size_t kFillChunk = std::size_t(1) << 20;     // 1 MiB prep chunks
constexpr std::uint64_t kTinyFileBytes = std::uint64_t(8) << 20;  // 8 MiB in tiny mode

bool tinyMode()
{
#ifdef SYNTHEON_DEV_HOOKS
    // The tiny run (small file + 10 ms windows) is for the unit tests and the
    // CI smoke launch (SYNTHEON_SMOKE=1), which must never write gigabytes or
    // run for minutes.
    return !qEnvironmentVariable("SYNTHEON_STORAGE_TINY").isEmpty()
        || !qEnvironmentVariable("SYNTHEON_SMOKE").isEmpty();
#else
    return false;
#endif
}

std::size_t depthFor(sb::storage::Pattern p)
{
    switch (p) {
    case sb::storage::Pattern::Seq1mQ8T1: return 8;
    case sb::storage::Pattern::Seq1mQ1T1: return 1;
    case sb::storage::Pattern::Rnd4kQ32T1: return 32;
    case sb::storage::Pattern::Rnd4kQ1T1: return 1;
    case sb::storage::Pattern::Mixed: return 32;
    default: return 1;
    }
}

std::size_t blockFor(sb::storage::Pattern p)
{
    return (p == sb::storage::Pattern::Seq1mQ8T1 || p == sb::storage::Pattern::Seq1mQ1T1) ? kSeqBlock : kRndBlock;
}

bool isSequential(sb::storage::Pattern p)
{
    return p == sb::storage::Pattern::Seq1mQ8T1 || p == sb::storage::Pattern::Seq1mQ1T1;
}

void closeFd(int fd)
{
#ifdef _WIN32
    ::_close(fd);
#else
    ::close(fd);
#endif
}

// The next `bytes` of the PRNG stream, as 64-bit words (§4.3).
void fillRandomNext(sb::cpu::Xoshiro256ss &rng, std::byte *buf, std::size_t bytes)
{
    std::uint64_t *words = reinterpret_cast<std::uint64_t *>(buf);
    const std::size_t n = bytes / 8;
    for (std::size_t i = 0; i < n; ++i)
        words[i] = rng.next();
    if (const std::size_t rem = bytes % 8) {
        const std::uint64_t last = rng.next();
        std::memcpy(words + n, &last, rem);
    }
}

double medianOf(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// A percentile of the samples (nearest-rank), in place on a sorted copy.
double percentileUs(std::vector<double> sorted, double pct)
{
    if (sorted.empty())
        return 0.0;
    std::sort(sorted.begin(), sorted.end());
    std::size_t rank = std::size_t(std::ceil(pct / 100.0 * sorted.size()));
    if (rank == 0)
        rank = 1;
    return sorted[rank - 1];
}

double cvPercent(const std::vector<double> &v)
{
    if (v.size() < 2)
        return 0.0;
    const double mean = std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
    double sq = 0;
    for (double x : v)
        sq += (x - mean) * (x - mean);
    const double sd = std::sqrt(sq / double(v.size()));
    return mean > 0 ? sd / mean * 100.0 : 0.0;
}

// One measured window's throughput.
struct Window {
    double mbps = 0;   // MB/s
    double iops = 0;
};

// The Storage run (the much smaller RamRun analogue, §5, §8): a direct
// steady_clock loop, no WorkerPool / core pinning.
class StorageRun
{
public:
    StorageRun(RunContext &ctx, const OptionValues &o)
        : m_ctx(ctx)
        , m_drive(o.value(QStringLiteral("drive")).toString())
        , m_fileSize(fileSizeBytes(o.value(QStringLiteral("file_size"), QStringLiteral("4g")).toString()))
        , m_offsetRng(kOffsetSeed)
    {
        m_tiny = tinyMode();
        // The declared size drives the free-space guard and the file_size
        // metric; tiny mode only shrinks the file actually written (and the
        // window), so a test run is fast without weakening the guard.
        m_actualFileSize = m_tiny ? kTinyFileBytes : m_fileSize;
        m_window = m_tiny ? std::chrono::milliseconds(10) : kWindow;
    }

    ~StorageRun() { cleanup(); }   // a crash/early-exit safety net

    TestResult run();

private:
    bool stopRequested()
    {
        if (m_ctx.cancelled())
            m_stopped = true;
        return m_stopped;
    }

    // Progress against the fixed budget, never backwards (P-PROG).
    void report(const QString &stage)
    {
        m_lastFraction = std::clamp(std::max(m_lastFraction, m_done / m_budgetTotal), 0.0, 0.999);
        m_ctx.progress(m_lastFraction, stage);
    }
    void step(const QString &stage)
    {
        m_done += 1.0;
        report(stage);
    }

    // The pre-run guard (§3.3, §3.5): an empty string means go.
    QString guard();

    bool prepare(QString *reason);
    bool fillFile(QString *reason);

    void runPattern(sb::storage::Pattern p);
    std::optional<Window> measureWindow(sb::storage::IStorageIoEngine &engine, sb::storage::IoOp op, sb::storage::Pattern p,
                                        std::size_t block, std::size_t depth, std::vector<double> *latencies,
                                        QString *error);
    std::optional<Window> measureMixed(sb::storage::IStorageIoEngine &engine, std::size_t block, std::size_t depth,
                                       QString *error);
    void latency();
    void finish(TestResult &r);
    void cleanup();

    // The engine for a pattern: sync (Q1T1), or the async backend at its depth.
    sb::storage::IStorageIoEngine &engineFor(sb::storage::Pattern p)
    {
        const std::size_t depth = depthFor(p);
        if (depth == 1)
            return *m_syncEngine;
        if (depth <= 8)
            return *m_q8Engine;
        return *m_q32Engine;
    }

    void metric(const QString &key, const QString &sub, const QString &label, double value, const QString &unit,
                Direction direction = Direction::HigherIsBetter, bool headline = false)
    {
        m_result.metrics.append({key, sub, label, value, unit, direction, headline});
    }
    void text(const QString &key, const QString &label, const QString &value)
    {
        m_result.metrics.append({key, QString(), label, value, QString(), Direction::Neutral, false});
    }

    RunContext &m_ctx;
    QString m_drive;
    std::uint64_t m_fileSize = 0;        // the declared size (guard + metric)
    std::uint64_t m_actualFileSize = 0;  // the file actually written
    bool m_tiny = false;
    std::chrono::milliseconds m_window{kWindow};

    // Chosen once per run (pickEngine), named in model.io: the sync engine for
    // Q1T1 and the async backend sized to Q8 and Q32 (§4.1: a pattern's
    // in-flight cap is its own depth).
    std::unique_ptr<sb::storage::IStorageIoEngine> m_syncEngine;
    std::unique_ptr<sb::storage::IStorageIoEngine> m_q8Engine;
    std::unique_ptr<sb::storage::IStorageIoEngine> m_q32Engine;
    QString m_engineName;

    // The over-aligned arena (§4.2), filled once with random data for writes.
    std::size_t m_align = 4096;
    std::vector<std::byte> m_arenaStorage;
    std::byte *m_arena = nullptr;

    QString m_targetDir;
    QString m_filePath;
    int m_fd = -1;
    double m_prepSeconds = 0;
    std::uint64_t m_bytesWritten = 0;   // the actual write footprint (prep + phases)

    // rnd4k_q1t1's read samples -> the latency percentiles (§2, P10).
    std::vector<double> m_latenciesUs;
    // The random-offset stream, seeded once per run so each window reads
    // different locations (a per-window reseed re-warms the same cache lines).
    sb::cpu::Xoshiro256ss m_offsetRng;

    double m_done = 0;
    double m_budgetTotal = 1;
    double m_lastFraction = 0;
    bool m_stopped = false;
    std::map<QString, Status> m_status;
    std::map<QString, QString> m_reason;
    double m_worstSpread = 0;
    TestResult m_result;
};

QString StorageRun::guard()
{
    if (m_drive.isEmpty())
        return QStringLiteral("no drive selected");
    // Read-only is checked live up front (the snapshot's storage field has no
    // read-only flag), so it fires whether the drive came from the snapshot or
    // the fallback.
    const QStorageInfo live(m_drive);
    if (live.isValid() && live.isReady() && live.isReadOnly())
        return QStringLiteral("the drive is read-only");
    // The selected drive from the run-start snapshot (§3.3: the run-time value
    // wins; the snapshot is what run() sees).
    const QVector<Drive> drives = listDrives(m_ctx.system());
    for (const Drive &d : drives) {
        if (d.mount != m_drive)
            continue;
        if (isBlockedDrive(d))
            return QStringLiteral("the drive is a network mount (blocked)");
        if (!sb::storage::hasEnoughFreeSpace(d.freeBytes, m_fileSize))
            return freeSpaceError(d.freeBytes, m_fileSize);
        return {};
    }
    // Not in the snapshot (or no snapshot storage field): fall back to the live
    // volume.
    const QStorageInfo info(m_drive);
    if (info.isValid() && info.isReady()) {
        Drive live2;
        live2.mount = m_drive;
        live2.fileSystem = QString::fromLatin1(info.fileSystemType());
        if (isBlockedDrive(live2))
            return QStringLiteral("the drive is a network mount (blocked)");
    }
    if (info.isValid() && info.isReady() && !sb::storage::hasEnoughFreeSpace(info.bytesAvailable(), m_fileSize))
        return freeSpaceError(info.bytesAvailable(), m_fileSize);
    return {};
}

bool StorageRun::prepare(QString *reason)
{
    m_syncEngine = sb::storage::pickEngine(sb::storage::IoDepth::One);
    m_q8Engine = sb::storage::pickEngine(sb::storage::IoDepth::Queued, 8);
    m_q32Engine = sb::storage::pickEngine(sb::storage::IoDepth::Queued, 32);
    m_engineName = m_q32Engine->backendName();

    // The arena: max depth x 1 MiB, over-aligned to the fs block size, filled
    // once with the PRNG (§4.2, §4.3). The write phases reuse it.
    m_align = sb::storage::directAlignment(m_drive);
    m_arenaStorage.resize(kMaxDepth * kSeqBlock + m_align);
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(m_arenaStorage.data());
    m_arena = reinterpret_cast<std::byte *>((base + m_align - 1) & ~std::uintptr_t(m_align - 1));
    sb::cpu::Xoshiro256ss rng(kSeed);
    fillRandomNext(rng, m_arena, kMaxDepth * kSeqBlock);

    // The hidden target folder (D5) and the file inside it. The mount root may
    // not be user-writable (e.g. "/" on a CI runner), so resolve a writable
    // directory on the same block device first (§3.2).
    const QString parent = writableParentOnDevice(m_drive);
    if (parent.isEmpty()) {
        *reason = QStringLiteral("no writable directory on the target device %1").arg(m_drive);
        return false;
    }
    m_targetDir = m_ctx.registerTargetScratchDir(parent);
    if (m_targetDir.isEmpty()) {
        *reason = QStringLiteral("could not create the target folder under %1").arg(parent);
        return false;
    }
    m_filePath = m_targetDir + QStringLiteral("/file.dat");

    auto fd = sb::storage::openDirectFile(m_filePath, /*read=*/true, /*write=*/true, reason);
    if (!fd)
        return false;   // §7: no silent buffered fallback
    m_fd = *fd;
    return true;
}

bool StorageRun::fillFile(QString *reason)
{
    const auto sync = sb::storage::makeSyncEngine();
    std::vector<std::byte> storage(kFillChunk + m_align);
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(storage.data());
    std::byte *buf = reinterpret_cast<std::byte *>((base + m_align - 1) & ~std::uintptr_t(m_align - 1));
    sb::cpu::Xoshiro256ss rng(kSeed);
    const auto stop = [this] { return m_ctx.cancelled(); };
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t offset = 0;
    while (offset < m_actualFileSize) {
        if (stop()) {
            m_stopped = true;   // latch: the run was cancelled, not "filled"
            return false;
        }
        const std::size_t n = std::size_t(std::min<std::uint64_t>(kFillChunk, m_actualFileSize - offset));
        fillRandomNext(rng, buf, n);
        sb::storage::IoView v{buf, n, offset};
        sb::storage::IoStats st;
        if (!sync->runBatch(&v, 1, sb::storage::IoOp::Write, m_fd, st, stop, reason))
            return false;
        offset += n;
        m_bytesWritten += st.bytes;
    }
    m_prepSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

std::optional<Window> StorageRun::measureWindow(sb::storage::IStorageIoEngine &engine, sb::storage::IoOp op,
                                               sb::storage::Pattern p, std::size_t block, std::size_t depth,
                                               std::vector<double> *latencies, QString *error)
{
    const std::uint64_t blocks = m_actualFileSize / block;
    // A batch larger than the in-flight cap so the sliding window stays full
    // (not count==depth, which drains to QD=0 between every runBatch), but with
    // a UNIQUE arena slot per view (`i`, not `i % depth`) so out-of-order
    // completions can't alias a slot still being DMA'd into, and bounded by the
    // 32 MiB arena. Q1T1 is sequential, so one view at a time suffices.
    const std::size_t batchSize = depth == 1 ? 1 : std::min(depth * 8, (kMaxDepth * kSeqBlock) / block);
    std::vector<sb::storage::IoView> batch(batchSize);
    std::uint64_t seqOff = 0;
    sb::storage::IoStats total;
    const auto t0 = std::chrono::steady_clock::now();
    const auto stop = [this] { return m_ctx.cancelled(); };
    while (!stop() && std::chrono::steady_clock::now() - t0 < m_window) {
        for (std::size_t i = 0; i < batchSize; ++i) {
            const std::uint64_t off = isSequential(p) ? (seqOff * block) % m_actualFileSize : m_offsetRng.below(blocks) * block;
            batch[i] = {m_arena + i * block, block, off};
            ++seqOff;
        }
        sb::storage::IoStats st;
        QString err;
        if (!engine.runBatch(batch.data(), batchSize, op, m_fd, st, stop, &err)) {
            if (error)
                *error = err.isEmpty() ? QStringLiteral("the I/O engine failed") : err;
            return std::nullopt;   // a real I/O failure: stop this pattern now
        }
        total.ops += st.ops;
        total.bytes += st.bytes;
        if (op == sb::storage::IoOp::Write)
            m_bytesWritten += st.bytes;
        if (latencies)
            latencies->insert(latencies->end(), st.latenciesUs.begin(), st.latenciesUs.end());
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double seconds = std::max(1e-9, std::chrono::duration<double>(t1 - t0).count());
    Window w;
    w.mbps = double(total.bytes) / seconds / 1'000'000.0;
    w.iops = double(total.ops) / seconds;
    return w;
}

std::optional<Window> StorageRun::measureMixed(sb::storage::IStorageIoEngine &engine, std::size_t block, std::size_t depth,
                                               QString *error)
{
    const std::uint64_t blocks = m_actualFileSize / block;
    std::vector<sb::storage::IoView> batch(depth);
    sb::storage::IoStats total;
    const auto t0 = std::chrono::steady_clock::now();
    const auto stop = [this] { return m_ctx.cancelled(); };
    // 70/30 random read/write (§2): ten single-round steps (7 read, 3 write),
    // the 5 s bound re-checked each round so a slow HDD/USB can't overshoot by
    // seconds.
    std::size_t step = 0;
    while (!stop() && std::chrono::steady_clock::now() - t0 < m_window) {
        const sb::storage::IoOp op = (step % 10) < 7 ? sb::storage::IoOp::Read : sb::storage::IoOp::Write;
        for (std::size_t i = 0; i < depth; ++i)
            batch[i] = {m_arena + i * block, block, m_offsetRng.below(blocks) * block};
        sb::storage::IoStats st;
        QString err;
        if (!engine.runBatch(batch.data(), depth, op, m_fd, st, stop, &err)) {
            if (error)
                *error = err.isEmpty() ? QStringLiteral("the I/O engine failed") : err;
            return std::nullopt;
        }
        total.ops += st.ops;
        total.bytes += st.bytes;
        if (op == sb::storage::IoOp::Write)
            m_bytesWritten += st.bytes;
        ++step;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double seconds = std::max(1e-9, std::chrono::duration<double>(t1 - t0).count());
    Window w;
    w.mbps = double(total.bytes) / seconds / 1'000'000.0;
    w.iops = double(total.ops) / seconds;
    return w;
}

void StorageRun::runPattern(sb::storage::Pattern p)
{
    const QString id = sb::storage::patternId(p);
    const std::size_t depth = depthFor(p);
    const std::size_t block = blockFor(p);
    sb::storage::IStorageIoEngine &engine = engineFor(p);
    m_ctx.subTestBegin(id);
    m_status[id] = Status::Ok;
    m_ctx.setPart(sb::storage::patternLabel(p));

    if (p == sb::storage::Pattern::Mixed) {
        std::vector<double> mbps, iops;
        QString err;
        for (int r = 0; r < kRuns && !stopRequested(); ++r) {
            step(QStringLiteral("Mixed 70/30 · run %1 of 3").arg(r + 1));
            const std::optional<Window> w = measureMixed(engine, block, depth, &err);
            if (!w) {
                m_status[id] = Status::Error;
                m_reason[id] = err;
                break;
            }
            mbps.push_back(w->mbps);
            iops.push_back(w->iops);
        }
        m_worstSpread = std::max(m_worstSpread, cvPercent(mbps));
        if (m_status[id] == Status::Ok && mbps.size() == kRuns) {
            metric(sb::storage::key(p, sb::storage::Field::Throughput), id, sb::storage::label(p, sb::storage::Field::Throughput),
                   medianOf(mbps), QStringLiteral("MB/s"), Direction::HigherIsBetter, true);
            metric(sb::storage::key(p, sb::storage::Field::Iops), id, sb::storage::label(p, sb::storage::Field::Iops),
                   medianOf(iops), QStringLiteral("IOPS"));
        } else if (m_stopped) {
            m_status[id] = Status::Cancelled;
        }
        m_ctx.subTestEnd(id, m_status[id]);
        return;
    }

    // Read phase (only rnd4k_q1t1 feeds the latency percentiles, §2 P10).
    std::vector<double> readMbps, readIops;
    std::vector<double> samples;
    QString err;
    for (int r = 0; r < kRuns && !stopRequested(); ++r) {
        step(QStringLiteral("%1 read · run %2 of 3").arg(id).arg(r + 1));
        const std::optional<Window> w =
            measureWindow(engine, sb::storage::IoOp::Read, p, block, depth,
                          p == sb::storage::Pattern::Rnd4kQ1T1 ? &samples : nullptr, &err);
        if (!w) {
            m_status[id] = Status::Error;
            m_reason[id] = err;
            break;
        }
        readMbps.push_back(w->mbps);
        readIops.push_back(w->iops);
    }
    // Write phase.
    std::vector<double> writeMbps, writeIops;
    for (int r = 0; r < kRuns && !stopRequested() && m_status[id] == Status::Ok; ++r) {
        step(QStringLiteral("%1 write · run %2 of 3").arg(id).arg(r + 1));
        const std::optional<Window> w = measureWindow(engine, sb::storage::IoOp::Write, p, block, depth, nullptr, &err);
        if (!w) {
            m_status[id] = Status::Error;
            m_reason[id] = err;
            break;
        }
        writeMbps.push_back(w->mbps);
        writeIops.push_back(w->iops);
    }
    m_worstSpread = std::max(m_worstSpread, std::max(cvPercent(readMbps), cvPercent(writeMbps)));

    if (m_status[id] == Status::Ok && readMbps.size() == kRuns && writeMbps.size() == kRuns) {
        metric(sb::storage::key(p, sb::storage::Field::Read), id, sb::storage::label(p, sb::storage::Field::Read),
               medianOf(readMbps), QStringLiteral("MB/s"), Direction::HigherIsBetter, p == sb::storage::Pattern::Seq1mQ8T1);
        metric(sb::storage::key(p, sb::storage::Field::Write), id, sb::storage::label(p, sb::storage::Field::Write),
               medianOf(writeMbps), QStringLiteral("MB/s"));
        if (!isSequential(p)) {
            metric(sb::storage::key(p, sb::storage::Field::ReadIops), id, sb::storage::label(p, sb::storage::Field::ReadIops),
                   medianOf(readIops), QStringLiteral("IOPS"), Direction::HigherIsBetter,
                   p == sb::storage::Pattern::Rnd4kQ32T1);
            metric(sb::storage::key(p, sb::storage::Field::WriteIops), id, sb::storage::label(p, sb::storage::Field::WriteIops),
                   medianOf(writeIops), QStringLiteral("IOPS"));
        }
        if (p == sb::storage::Pattern::Rnd4kQ1T1)
            m_latenciesUs = std::move(samples);   // the latency source (§2, P10)
    } else if (m_stopped) {
        m_status[id] = Status::Cancelled;
    }
    m_ctx.subTestEnd(id, m_status[id]);
}

void StorageRun::latency()
{
    const QString id = QStringLiteral("latency");
    m_ctx.subTestBegin(id);
    m_status[id] = Status::Ok;
    m_ctx.setPart(QStringLiteral("Latency (4K Q1T1)"));
    metric(sb::storage::key(sb::storage::Pattern::Latency, sb::storage::Field::P50), id,
           sb::storage::label(sb::storage::Pattern::Latency, sb::storage::Field::P50),
           percentileUs(m_latenciesUs, 50.0), QStringLiteral("µs"), Direction::LowerIsBetter, true);
    metric(sb::storage::key(sb::storage::Pattern::Latency, sb::storage::Field::P99), id,
           sb::storage::label(sb::storage::Pattern::Latency, sb::storage::Field::P99),
           percentileUs(m_latenciesUs, 99.0), QStringLiteral("µs"), Direction::LowerIsBetter);
    metric(sb::storage::key(sb::storage::Pattern::Latency, sb::storage::Field::P999), id,
           sb::storage::label(sb::storage::Pattern::Latency, sb::storage::Field::P999),
           percentileUs(m_latenciesUs, 99.9), QStringLiteral("µs"), Direction::LowerIsBetter);
    m_ctx.subTestEnd(id, Status::Ok);
}

void StorageRun::finish(TestResult &r)
{
    // The neutral metrics (§10) go onto the moved-out result `r` (the pattern
    // metrics were appended to it before the move).
    const auto num = [&r](const QString &key, const QString &label, double value, const QString &unit) {
        r.metrics.append({key, QString(), label, value, unit, Direction::Neutral, false});
    };
    const auto str = [&r](const QString &key, const QString &label, const QString &value) {
        r.metrics.append({key, QString(), label, value, QString(), Direction::Neutral, false});
    };
    str(sb::storage::kModelIo, QStringLiteral("I/O engine"), m_engineName);
    str(sb::storage::kTargetDrive, QStringLiteral("Target drive"), m_drive);
    QString fs, kind;
    for (const Drive &d : listDrives(m_ctx.system())) {
        if (d.mount == m_drive) {
            fs = d.fileSystem;
            kind = d.kind;
            num(sb::storage::kTargetFreeBytes, QStringLiteral("Free space"),
                double(d.freeBytes) / double(std::uint64_t(1) << 30), QStringLiteral("GiB"));
            break;
        }
    }
    str(sb::storage::kTargetFilesystem, QStringLiteral("Filesystem"), fs);
    str(sb::storage::kTargetKind, QStringLiteral("Drive kind"), kind);
    num(sb::storage::kFileSize, QStringLiteral("Test file size"),
        double(m_fileSize) / double(std::uint64_t(1) << 30), QStringLiteral("GiB"));
    num(sb::storage::kDirectAlignment, QStringLiteral("Direct-I/O alignment"), double(m_align), QStringLiteral("B"));
    num(sb::storage::kPrepSeconds, QStringLiteral("File preparation"), m_prepSeconds, QStringLiteral("s"));
    num(sb::storage::kWearWriteGb, QStringLiteral("Bytes written"),
        double(m_bytesWritten) / 1'000'000'000.0, QStringLiteral("GB"));
    num(sb::storage::kStabilitySpread, QStringLiteral("Worst window CV"), m_worstSpread, QStringLiteral("%"));
}

void StorageRun::cleanup()
{
    if (m_fd >= 0) {
        closeFd(m_fd);
        m_fd = -1;
    }
    // Remove the target folder and file (normal completion, cancel, error and
    // crash), never following a link (P6).
    if (!m_targetDir.isEmpty() && !sb::fs::removeTreeNoFollow(m_targetDir))
        m_ctx.log(LogLevel::Warning, QStringLiteral("Couldn't fully delete the test folder %1; the run's cleanup will retry")
                                          .arg(m_targetDir));
    m_targetDir.clear();
}

TestResult StorageRun::run()
{
    // §3.5 blocklist / §3.3 free-space guard, read from the run-start snapshot.
    if (const QString why = guard(); !why.isEmpty()) {
        m_result = TestResult::notAvailable(why);
        m_result.subResults = {};
        return m_result;
    }

    // The budget: prep + 4 patterns x 2 phases x 3 runs + mixed x 3 runs +
    // latency = 1 + 24 + 3 + 1 = 29 steps (the stage labels drive the UI).
    m_budgetTotal = 1.0 + 4.0 * 2.0 * kRuns + 1.0 * kRuns + 1.0;

    QString reason;
    if (!prepare(&reason)) {
        cleanup();
        if (stopRequested())
            return TestResult::withStatus(Status::Cancelled, defaultStoppedReason());
        // openDirectFile failure = NotAvailable (§7), never a silent fallback.
        m_result = TestResult::notAvailable(reason);
        return m_result;
    }
    step(QStringLiteral("Preparing the test file"));
    m_ctx.log(LogLevel::Info, QStringLiteral("Starting Storage: %1 → %2, %3, backend %4")
                                  .arg(m_drive, m_filePath,
                                       QStringLiteral("%1 GiB").arg(double(m_fileSize) / double(std::uint64_t(1) << 30), 0, 'f', 1),
                                       m_engineName));
    if (!fillFile(&reason)) {
        cleanup();
        if (m_stopped)
            return TestResult::withStatus(Status::Cancelled, defaultStoppedReason());
        m_result = TestResult::error(reason);
        return m_result;
    }

    const sb::storage::Pattern order[] = {sb::storage::Pattern::Seq1mQ8T1, sb::storage::Pattern::Seq1mQ1T1,
                                          sb::storage::Pattern::Rnd4kQ32T1, sb::storage::Pattern::Rnd4kQ1T1,
                                          sb::storage::Pattern::Mixed};
    for (sb::storage::Pattern p : order) {
        if (stopRequested())
            break;
        runPattern(p);
    }
    if (!stopRequested()) {
        if (!m_latenciesUs.empty()) {
            latency();
        } else {
            // No rnd4k_q1t1 read samples: record latency as NotAvailable rather
            // than silently omitting the sub-test (design §2: latency is derived).
            const QString id = QStringLiteral("latency");
            m_ctx.subTestBegin(id);
            m_status[id] = Status::NotAvailable;
            m_reason[id] = QStringLiteral("no latency samples (rnd4k_q1t1 read did not run)");
            m_ctx.subTestEnd(id, Status::NotAvailable);
        }
    }

    TestResult r = std::move(m_result);
    finish(r);
    // Sub-results in the canonical pattern order (§10 / kPatterns), not the
    // alphabetical order of the status map.
    for (sb::storage::Pattern p : sb::storage::kPatterns) {
        const QString id = sb::storage::patternId(p);
        const auto it = m_status.find(id);
        if (it == m_status.end())
            continue;   // never started (Stop)
        r.subResults.append({id, it->second, m_reason[id], {}});
    }

    if (m_stopped) {
        r.status = Status::Cancelled;
    } else {
        int ok = 0;
        for (const SubResult &s : std::as_const(r.subResults))
            if (s.status == Status::Ok)
                ++ok;
        if (ok == 6)
            r.status = Status::Ok;
        else if (ok > 0) {
            r.status = Status::Partial;
            r.statusReason = QStringLiteral("No Storage score: not every part ran");
        } else {
            r.status = Status::Error;
            r.statusReason = QStringLiteral("the Storage test failed");
        }
    }

    cleanup();
    if (!m_stopped)
        m_ctx.progress(1.0, QStringLiteral("Done"));
    return r;
}

} // namespace

StorageTest::StorageTest()
{
    m_info.id = QStringLiteral("storage");
    m_info.name = QStringLiteral("Storage");
    m_info.summary = QStringLiteral("Sequential and random read/write, latency, mixed");
    m_info.colorToken = QStringLiteral("t-sto");
    // 1: this is the real measurement (replaces the stub). The formula's
    // anchors are frozen at Tiamat's NVMe (task 13), so formulaVersion is 1.
    m_info.testVersion = 1;
    m_info.formulaVersion = 1;
    // D15: every run runs all six; none is selectable.
    m_info.subTests = {{QStringLiteral("seq1m_q8t1"), QStringLiteral("Seq 1M Q8T1"), false, true},
                       {QStringLiteral("seq1m_q1t1"), QStringLiteral("Seq 1M Q1T1"), false, true},
                       {QStringLiteral("rnd4k_q32t1"), QStringLiteral("Random 4K Q32T1"), false, true},
                       {QStringLiteral("rnd4k_q1t1"), QStringLiteral("Random 4K Q1T1"), false, true},
                       {QStringLiteral("latency"), QStringLiteral("Latency (4K Q1T1)"), false, true},
                       {QStringLiteral("mixed"), QStringLiteral("Mixed 70/30 random"), false, true}};
}

Availability StorageTest::probe(const ProbeContext &ctx) const
{
    Availability a;
    const QVector<Drive> drives = listDrives(ctx.system());
    a.capabilities.append({QStringLiteral("%1 drive%2")
                               .arg(drives.size())
                               .arg(drives.size() == 1 ? QString() : QStringLiteral("s")),
                           Tone::Neutral});
    // The async backend this platform compiled (io_uring / libaio / overlapped / sync).
    const std::unique_ptr<sb::storage::IStorageIoEngine> engine =
        sb::storage::pickEngine(sb::storage::IoDepth::Queued);
    a.capabilities.append({engine->backendName(), Tone::Neutral});
    int blocked = 0;
    for (const Drive &d : drives)
        if (isBlockedDrive(d))
            ++blocked;
    if (blocked)
        a.capabilities.append({QStringLiteral("%1 network mount%2 blocked")
                                   .arg(blocked)
                                   .arg(blocked == 1 ? QString() : QStringLiteral("s")),
                               Tone::Warn});
    return a;
}

OptionSchema StorageTest::options(const ProbeContext &ctx, const Availability &) const
{
    OptionSchema o;

    OptionDef drive;
    drive.key = kDriveKey;
    drive.label = QStringLiteral("Drive");
    drive.kind = OptionKind::DynamicSelect;
    QString firstEnabled;
    for (const Drive &d : listDrives(ctx.system())) {
        OptionItem item;
        item.value = d.mount;
        item.label = d.model.isEmpty() ? d.mount : QStringLiteral("%1 (%2)").arg(d.mount, d.model);
        item.detail = QStringLiteral("%1 · %2 GB free · %3")
                          .arg(d.fileSystem.isEmpty() ? QStringLiteral("?") : d.fileSystem)
                          .arg(double(d.freeBytes) / 1'000'000'000.0, 0, 'f', 0)
                          .arg(d.kind.isEmpty() ? QStringLiteral("?") : d.kind);
        if (isBlockedDrive(d)) {
            item.enabled = false;
            item.disabledReason = sb::storage::networkBlockReason(d.fileSystem);
        }
        drive.items.append(item);
        if (firstEnabled.isEmpty() && item.enabled)
            firstEnabled = item.value;
    }
    drive.defaultValue = firstEnabled;
    o.options.append(drive);

    OptionDef size;
    size.key = kFileSizeKey;
    size.label = QStringLiteral("Test file size");
    size.kind = OptionKind::SingleSelect;
    size.presentation = SelectPresentation::Segmented;
    size.items = {{QStringLiteral("1g"), QStringLiteral("1 GiB")},
                  {QStringLiteral("4g"), QStringLiteral("4 GiB")},
                  {QStringLiteral("16g"), QStringLiteral("16 GiB")},
                  {QStringLiteral("64g"), QStringLiteral("64 GiB")}};
    size.defaultValue = QStringLiteral("4g");
    o.options.append(size);

    OptionDef engine;
    engine.key = QStringLiteral("engine");
    engine.label = QStringLiteral("I/O engine");
    engine.kind = OptionKind::Info;
    engine.infoValue = sb::storage::pickEngine(sb::storage::IoDepth::Queued)->backendName();
    o.options.append(engine);

    OptionDef cache;
    cache.key = QStringLiteral("cache");
    cache.label = QStringLiteral("Cache");
    cache.kind = OptionKind::Info;
    cache.infoValue = QStringLiteral("bypassed (O_DIRECT / unbuffered I/O)");
    o.options.append(cache);

    return o;
}

QList<Precondition> StorageTest::preconditions(const OptionValues &o, const ProbeContext &ctx) const
{
    QList<Precondition> out;
    const QString sizeKey = o.value(kFileSizeKey, QStringLiteral("4g")).toString();
    const qint64 size = fileSizeBytes(sizeKey);
    const QString drive = o.value(kDriveKey).toString();

    // The wear note: prep (one full write) + one more full write during the
    // measured write phases (§3.4: a 4 GiB file -> ~8 GB). Informational.
    std::optional<Drive> sel;
    for (const Drive &d : listDrives(ctx.system()))
        if (d.mount == drive)
            sel = d;   // copy: the temporary QVector is gone after the loop
    Precondition wear;
    wear.id = QStringLiteral("wear");
    wear.label = QStringLiteral("Drive wear");
    wear.state = Precondition::State::Met;
    wear.detail = sb::storage::wearNote(sb::storage::conservativeWriteBytes(size), sel ? sel->kind : QString());
    out.append(wear);

    // D2: a removable / optical drive is warned, not blocked.
    if (sel && isRemovableDrive(*sel)) {
        Precondition removable;
        removable.id = QStringLiteral("removable");
        removable.label = QStringLiteral("Removable drive");
        removable.state = Precondition::State::Met;
        removable.detail = QStringLiteral("this is a removable / optical drive; results may vary between reconnects");
        out.append(removable);
    }
    return out;
}

Validation StorageTest::validate(const OptionValues &o) const
{
    Validation v;
    const QString drive = o.value(kDriveKey).toString();
    const QString sizeKey = o.value(kFileSizeKey, QStringLiteral("4g")).toString();
    const qint64 size = fileSizeBytes(sizeKey);

    if (drive.isEmpty()) {
        v.add(kDriveKey, QStringLiteral("choose a drive"));
        return v;
    }

    // The free-space guard (§3.3), read live so a disk that filled since probe
    // is caught before start; a read-only mount is rejected outright.
    const QStorageInfo info(drive);
    if (info.isValid() && info.isReady()) {
        if (info.isReadOnly())
            v.add(kDriveKey, QStringLiteral("the drive is read-only"));
        else {
            const QString refusal = freeSpaceError(info.bytesAvailable(), size);
            if (!refusal.isEmpty())
                v.add(kDriveKey, refusal);
        }
    }
    return v;
}

std::chrono::seconds StorageTest::estimate(const OptionValues &o) const
{
    // §2.2: prep at a conservative 200 MB/s + 180 s of measurement + ~3 s of
    // warm-up (one short window per pattern). Size-scaled, never zero.
    const qint64 size = fileSizeBytes(o.value(kFileSizeKey, QStringLiteral("4g")).toString());
    const double secs = double(size) / 200'000'000.0 + 180.0 + 3.0;
    return std::chrono::seconds(std::max<long long>(1, std::llround(secs)));
}

std::chrono::seconds StorageTest::timeLimit(const OptionValues &o) const
{
    // 2 x the estimate + 60 s (§12). Storage windows are wall-clock (§9: no
    // stub time scale), so there is no stub-scale floor.
    return std::chrono::seconds(2 * estimate(o).count() + 60);
}

TestResult StorageTest::run(RunContext &ctx, const OptionValues &o)
{
    StorageRun r(ctx, o);
    return r.run();
}

ScoreBreakdown StorageTest::score(const TestResult &result) const
{
    return sb::storage::formula_v1::score(result);
}

} // namespace sb::tests
