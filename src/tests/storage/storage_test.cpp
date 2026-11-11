// SPDX-License-Identifier: GPL-3.0-or-later
// The Storage test (Phase 4b design): info/probe/options/preconditions/
// validate/estimate/timeLimit and score; the run loop is §8 (task 11) and the
// Detail page is storage_detail.cpp. The drive picker reads the snapshot's
// `storage` array (§3.1, N1), falling back to QStorageInfo::mountedVolumes();
// the free-space guard, blocklist and wear note live in req.{h,cpp} (§6).
#include "tests/storage/storage_test.h"

#include "tests/storage/formula_v1.h"
#include "tests/storage/ids.h"
#include "tests/storage/io/engine.h"
#include "tests/storage/req.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStorageInfo>

#include <cmath>

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

} // namespace

StorageTest::StorageTest()
{
    m_info.id = QStringLiteral("storage");
    m_info.name = QStringLiteral("Storage");
    m_info.summary = QStringLiteral("Sequential and random read/write, latency, mixed");
    m_info.colorToken = QStringLiteral("t-sto");
    // 1: this is the real measurement (replaces the stub). The formula's
    // anchors are still placeholders, so formulaVersion stays 0 until task 13.
    m_info.testVersion = 1;
    m_info.formulaVersion = 0;
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
        if (sb::storage::isNetworkMount(d.mount, d.fileSystem))
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
        if (sb::storage::isNetworkMount(d.mount, d.fileSystem)) {
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
    QString kind;
    for (const Drive &d : listDrives(ctx.system()))
        if (d.mount == drive)
            kind = d.kind;
    Precondition wear;
    wear.id = QStringLiteral("wear");
    wear.label = QStringLiteral("Drive wear");
    wear.state = Precondition::State::Met;
    wear.detail = sb::storage::wearNote(sb::storage::worstCaseWriteBytes(size, 1), kind);
    out.append(wear);
    return out;
}

Validation StorageTest::validate(const OptionValues &o) const
{
    Validation v;
    const QString drive = o.value(kDriveKey).toString();
    const QString sizeKey = o.value(kFileSizeKey, QStringLiteral("4g")).toString();
    const qint64 size = fileSizeBytes(sizeKey);

    // The free-space guard (§3.3), read live so a disk that filled since probe
    // is caught before start.
    const QStorageInfo info(drive);
    if (!drive.isEmpty() && info.isValid() && info.isReady()) {
        const QString refusal = freeSpaceError(info.bytesAvailable(), size);
        if (!refusal.isEmpty())
            v.add(kDriveKey, refusal);
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

TestResult StorageTest::run(RunContext &, const OptionValues &)
{
    // Implemented in task 11 (StorageRun).
    return TestResult::error(QStringLiteral("Storage run not implemented"));
}

ScoreBreakdown StorageTest::score(const TestResult &result) const
{
    return sb::storage::formula_v1::score(result);
}

} // namespace sb::tests
