// SPDX-License-Identifier: GPL-3.0-or-later
// The assembled Storage test (Phase 4b design §3, §12, §15.1): the drive
// picker from the snapshot's `storage` field, the blocked (network) drive
// disabled with its reason, the free-space guard, the file-size option, and
// (task 11) a stub-scale run against a temp dir. Mirrors tst_ram_test.cpp.
#include "tests/storage/storage_test.h"
#include "tests/storage/req.h"
#include "privileged/privileged_ops.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <mutex>

using namespace sb;
using namespace sb::tests;

using Clock = std::chrono::steady_clock;

namespace {

// A snapshot with two volumes: the root (SSD) and a network mount.
SystemSnapshot storageSnapshot()
{
    SystemSnapshot s;
    s.data.insert(QStringLiteral("storage"),
                  QJsonArray{
                      QJsonObject{{QStringLiteral("mount"), QStringLiteral("/")},
                                  {QStringLiteral("file_system"), QStringLiteral("btrfs")},
                                  {QStringLiteral("free_bytes"), 500.0e9},
                                  {QStringLiteral("total_bytes"), 1.0e12},
                                  {QStringLiteral("model"), QStringLiteral("Samsung 990 Pro")},
                                  {QStringLiteral("bus"), QStringLiteral("NVMe")},
                                  {QStringLiteral("kind"), QStringLiteral("SSD")}},
                      QJsonObject{{QStringLiteral("mount"), QStringLiteral("/mnt/nas")},
                                  {QStringLiteral("file_system"), QStringLiteral("nfs4")},
                                  {QStringLiteral("free_bytes"), 1.0e12},
                                  {QStringLiteral("total_bytes"), 4.0e12},
                                  {QStringLiteral("model"), QStringLiteral("NAS")},
                                  {QStringLiteral("bus"), QString()},
                                  {QStringLiteral("kind"), QString()}},
                  });
    return s;
}

struct Ctx : ProbeContext {
    SystemSnapshot s = storageSnapshot();
    const SystemSnapshot &system() const override { return s; }
    PrivilegedOps &privileged() const override { return const_cast<UnavailablePrivilegedOps &>(p); }

private:
    UnavailablePrivilegedOps p;
};

// A snapshot whose only drive is `mount` (for the run tests).
SystemSnapshot snapshotWithDrive(const QString &mount, qint64 freeBytes, const QString &fs, const QString &kind)
{
    SystemSnapshot s;
    s.data.insert(QStringLiteral("storage"),
                  QJsonArray{QJsonObject{{QStringLiteral("mount"), mount},
                                         {QStringLiteral("file_system"), fs},
                                         {QStringLiteral("free_bytes"), double(freeBytes)},
                                         {QStringLiteral("total_bytes"), double(freeBytes) * 2},
                                         {QStringLiteral("model"), QStringLiteral("Test drive")},
                                         {QStringLiteral("bus"), QStringLiteral("NVMe")},
                                         {QStringLiteral("kind"), kind}}});
    return s;
}

// A run context for the assembled test (§15.1; the small tst_ram_test FakeRunContext analogue).
class FakeRunContext : public RunContext
{
public:
    void progress(double fraction, const QString &stage) override
    {
        std::lock_guard lock(m_mutex);
        if (!m_progress.isEmpty() && fraction + 1e-9 < m_progress.last().first)
            m_backwards = true;
        m_progress.append({fraction, stage});
        if (!m_cancelOn.isEmpty() && stage.startsWith(m_cancelOn) && !m_cancel) {
            m_cancelAt = Clock::now();
            m_cancel = true;
        }
    }
    void log(LogLevel, const QString &message) override
    {
        std::lock_guard lock(m_mutex);
        m_logs.append(message);
    }
    void setPart(const QString &part) override
    {
        std::lock_guard lock(m_mutex);
        m_parts.append(part);
    }
    bool cancelled() const override { return m_cancel.load(); }
    void subTestBegin(const QString &id) override
    {
        std::lock_guard lock(m_mutex);
        m_subTests.append(id + QStringLiteral(":begin"));
    }
    void subTestEnd(const QString &id, Status status) override
    {
        std::lock_guard lock(m_mutex);
        m_subTests.append(id + QStringLiteral(":") + toKey(status));
    }
    void checkpoint(const QCborMap &) override {}
    QString scratchDir() override { return {}; }
    const SystemSnapshot &system() const override { return m_system; }
    PrivilegedOps &privileged() override { return m_priv; }

    void setSystem(const SystemSnapshot &s) { m_system = s; }
    void cancelOn(const QString &stage) { m_cancelOn = stage; }
    Clock::time_point cancelAt() const { return m_cancelAt.load(); }
    bool wentBackwards() const { return m_backwards; }

private:
    mutable std::mutex m_mutex;
    QList<QPair<double, QString>> m_progress;
    QStringList m_logs;
    QStringList m_subTests;
    QStringList m_parts;
    std::atomic<bool> m_cancel{false};
    std::atomic<Clock::time_point> m_cancelAt{};
    QString m_cancelOn;
    bool m_backwards = false;
    SystemSnapshot m_system;
    UnavailablePrivilegedOps m_priv;
};

} // namespace

class TestStorageTest : public QObject
{
    Q_OBJECT

private slots:
    void identity()
    {
        StorageTest t;
        QCOMPARE(t.info().id, QStringLiteral("storage"));
        QCOMPARE(t.info().name, QStringLiteral("Storage"));
        QCOMPARE(t.info().colorToken, QStringLiteral("t-sto"));
        QCOMPARE(t.info().testVersion, 1);
        QStringList subs;
        for (const SubTestInfo &s : t.info().subTests) {
            subs << s.id;
            QVERIFY(!s.userSelectable);   // D15: all six, none selectable
        }
        QCOMPARE(subs, (QStringList{QStringLiteral("seq1m_q8t1"), QStringLiteral("seq1m_q1t1"),
                                    QStringLiteral("rnd4k_q32t1"), QStringLiteral("rnd4k_q1t1"),
                                    QStringLiteral("latency"), QStringLiteral("mixed")}));
    }

    void pickerAndOptions()
    {
        StorageTest t;
        Ctx ctx;
        const Availability a = t.probe(ctx);
        QVERIFY(a.state != Availability::State::Unavailable);

        const OptionSchema s = effectiveOptionSchema(t, ctx, a);
        const OptionDef *drive = s.find(QStringLiteral("drive"));
        QVERIFY(drive);
        QCOMPARE(drive->kind, OptionKind::DynamicSelect);
        QCOMPARE(drive->items.size(), 2);
        // The root volume is enabled and default.
        QCOMPARE(drive->items[0].value, QStringLiteral("/"));
        QVERIFY(drive->items[0].enabled);
        QCOMPARE(drive->defaultValue.toString(), QStringLiteral("/"));
        // The network mount is listed but disabled, with its reason.
        QCOMPARE(drive->items[1].value, QStringLiteral("/mnt/nas"));
        QVERIFY(!drive->items[1].enabled);
        QCOMPARE(drive->items[1].disabledReason, QStringLiteral("nfs4 · network — blocked"));

        const OptionDef *size = s.find(QStringLiteral("file_size"));
        QVERIFY(size);
        QCOMPARE(size->kind, OptionKind::SingleSelect);
        QCOMPARE(size->presentation, SelectPresentation::Segmented);
        QStringList values;
        for (const OptionItem &i : size->items)
            values << i.value;
        QCOMPARE(values, (QStringList{QStringLiteral("1g"), QStringLiteral("4g"), QStringLiteral("16g"), QStringLiteral("64g")}));
        QCOMPARE(size->defaultValue.toString(), QStringLiteral("4g"));
        // The stub's target_folder option is retired (D5), and no subtests checklist.
        QVERIFY(!s.find(QStringLiteral("target_folder")));
        QVERIFY(!s.find(QStringLiteral("subtests")));
        // The engine Info row names the compiled backend.
        QVERIFY(s.find(QStringLiteral("engine")));
    }

    void freeSpaceRefusalString()
    {
        StorageTest t;
        // validate() reads the free space live (QStorageInfo); the exact string
        // it emits is the pure req.h helper, checked here deterministically.
        const QString refusal = sb::storage::freeSpaceRefusal(4'400'000'000ll, 4'000'000'000ll);
        QCOMPARE(refusal, QStringLiteral("Not enough free space: the test needs about 6.1 GiB, the drive has 4 GiB"));
        // validate() on an empty drive is clean (no drive selected -> no guard).
        QVERIFY(t.validate({}).ok());
    }

    // A small (tiny-mode) run against a QTemporaryDir-as-drive, §15.1/§14:
    // metrics present, all six sub-results Ok, progress monotonic, and Stop
    // within 250 ms removes the file.
    void tinyRunAndStop()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qputenv("SYNTHEON_STORAGE_TINY", "1");

        StorageTest t;
        FakeRunContext ctx;
        ctx.setSystem(snapshotWithDrive(dir.path(), 1'000'000'000'000ll, QStringLiteral("tmpfs"), QStringLiteral("SSD")));
        const OptionValues o{{QStringLiteral("drive"), dir.path()}, {QStringLiteral("file_size"), QStringLiteral("1g")}};
        const TestResult r = t.run(ctx, o);

        // All six sub-results ran Ok (or, if the fixture fs refused O_DIRECT,
        // the whole run is NotAvailable — the tolerent tmpfs case).
        if (r.status == Status::NotAvailable) {
            qInfo("O_DIRECT unavailable on the fixture fs: %s", qPrintable(r.statusReason));
            return;
        }
        QCOMPARE(r.status, Status::Ok);
        QCOMPARE(r.subResults.size(), 6);
        for (const SubResult &s : r.subResults)
            QCOMPARE(s.status, Status::Ok);
        QVERIFY(r.metric(QStringLiteral("seq1m_q8t1.read")));
        QVERIFY(r.metric(QStringLiteral("rnd4k_q32t1.read_iops")));
        QVERIFY(r.metric(QStringLiteral("mixed.throughput")));
        QVERIFY(r.metric(QStringLiteral("latency.p50")));
        QVERIFY(r.metric(QStringLiteral("model.io")));
        QVERIFY(!ctx.wentBackwards());
        // The target folder + file are gone (no hidden .syntheon-storage-* left).
        const QStringList dirs = QDir(dir.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QVERIFY2(dirs.isEmpty(), qPrintable(QStringLiteral("leftover dirs: ") + dirs.join(QStringLiteral(", "))));
        qunsetenv("SYNTHEON_STORAGE_TINY");
    }

    void stopRemovesTheFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qputenv("SYNTHEON_STORAGE_TINY", "1");

        StorageTest t;
        FakeRunContext ctx;
        ctx.setSystem(snapshotWithDrive(dir.path(), 1'000'000'000'000ll, QStringLiteral("tmpfs"), QStringLiteral("SSD")));
        ctx.cancelOn(QStringLiteral("seq1m_q8t1 read"));
        const OptionValues o{{QStringLiteral("drive"), dir.path()}, {QStringLiteral("file_size"), QStringLiteral("1g")}};
        const TestResult r = t.run(ctx, o);

        QVERIFY(ctx.cancelAt() != Clock::time_point{});
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - ctx.cancelAt()).count();
        qInfo("Stop took %.1f ms", ms);
        QVERIFY2(ms < 250, qPrintable(QStringLiteral("Stop took %1 ms").arg(ms, 0, 'f', 1)));
        QCOMPARE(r.status, Status::Cancelled);
        // The target folder + file are removed on Stop.
        const QStringList dirs = QDir(dir.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QVERIFY2(dirs.isEmpty(), qPrintable(QStringLiteral("leftover dirs: ") + dirs.join(QStringLiteral(", "))));
        qunsetenv("SYNTHEON_STORAGE_TINY");
    }
};

QTEST_GUILESS_MAIN(TestStorageTest)
#include "tst_storage_test.moc"
