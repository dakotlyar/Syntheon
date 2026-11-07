// SPDX-License-Identifier: GPL-3.0-or-later
//
// Moving people's history and settings across the rename. Every path here is a
// QTemporaryDir or a Qt test-mode location, and on Unix the runtime folder is a
// QTemporaryDir too (initTestCase), so the old build's lock folder is never the
// real one even outside CTest.
#include "storage/database.h"
#include "storage/instance_paths.h"
#include "storage/legacy_migration.h"
#include "storage/legacy_names.h"
#include "storage/run_repository.h"
#include "storage/settings_store.h"
#include "storage/storage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QScopeGuard>
#include <QSettings>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace sb;

namespace {

// The folder the old build keeps its locks in.
QString legacyLockDir()
{
#ifdef Q_OS_WIN
    return QDir(legacy::defaultDataDir()).filePath(QStringLiteral("run"));
#else
    return QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)).filePath(QString::fromLatin1(legacy::kRuntimeDirName));
#endif
}

constexpr QFile::Permissions kReadOnly = QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther;

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

// An old settings.ini, as the old build's SettingsStore wrote it.
void writeLegacyIni(const QString &path, const QString &dataLocation)
{
    SettingsStore s(path);
    s.setDataLocation(dataLocation);
    s.setLayout(QStringLiteral("tabs"));
    s.setTheme(QStringLiteral("dark"));
    s.setFirstRunDone(true);
    QVERIFY(s.sync());
}

QVariantMap allValues(const QString &ini)
{
    QSettings s(ini, QSettings::IniFormat);
    QVariantMap m;
    for (const QString &key : s.allKeys())
        m.insert(key, s.value(key));
    return m;
}

RunRecord sampleRun(double overall)
{
    RunRecord r;
    r.uuid = QUuid::createUuid();
    r.kind = RunKind::Standard;
    r.status = RunStatus::Completed;
    r.startedAt = QDateTime(QDate(2026, 10, 24), QTime(14, 2, 3, 456), QTimeZone::UTC);
    r.finishedAt = r.startedAt.addSecs(180);
    r.appVersion = QStringLiteral("2026.11.0");
    r.appBuild = QStringLiteral("linux-x64 abc1234");
    r.system.machineLabel = QStringLiteral("Tiamat");
    r.system.data.insert(QStringLiteral("cpu"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Ryzen")}});
    r.overall = overall;

    TestResult gpu;
    gpu.testId = QStringLiteral("gpu");
    gpu.testVersion = 1;
    gpu.formulaVersion = 1;
    gpu.status = Status::Ok;
    gpu.score = overall - 1;
    gpu.options = {{QStringLiteral("profile"), QStringLiteral("mixed")}, {QStringLiteral("duration"), 30.5}};
    gpu.startedAt = r.startedAt.addMSecs(10);
    gpu.durationMs = 120034;
    gpu.metrics = {
        {QStringLiteral("fps"), {}, QStringLiteral("Frame rate"), 152.25, QStringLiteral("fps"), Direction::HigherIsBetter, true},
        {QStringLiteral("shaders"), {}, QStringLiteral("Shaders"), QStringLiteral("c957"), QString(), Direction::Neutral, false},
    };
    gpu.series = {{QStringLiteral("curve"), QStringLiteral("Curve"), QStringLiteral("Size"), QStringLiteral("KiB"),
                   QStringLiteral("ns"), {{4, 1.25}, {8, -2.5}, {1e9, 3.141592653589793}}}};
    r.tests = {gpu};
    return r;
}

// A history at `path` with `runs` runs, closed, as a cleanly quit build leaves it.
QVector<RunRecord> makeHistory(const QString &path, int runs)
{
    QVector<RunRecord> records;
    if (!Database::migrate(path).ok())
        return records;
    Database::ScopedConnection c(path, QStringLiteral("seed"));
    for (int i = 0; i < runs; ++i) {
        records.append(sampleRun(1000.5 + i));
        if (!RunRepository(c.db()).saveRun(records.last()))
            records.removeLast();
    }
    return records;
}

int runsIn(const QString &path)
{
    Database::ScopedConnection c(path, QStringLiteral("count"));
    return RunRepository(c.db()).runCount();
}

// Every row of every stored table, blobs as hex: equal means nothing was reinterpreted.
QStringList allRows(const QString &path)
{
    QStringList rows;
    Database::ScopedConnection c(path, QStringLiteral("dump"));
    for (const char *table : {"runs", "test_results", "sub_results", "metrics", "series", "checkpoints", "run_log", "scratch_paths"}) {
        QSqlQuery q(c.db());
        if (!q.exec(QStringLiteral("SELECT * FROM %1 ORDER BY id").arg(QLatin1StringView(table))))
            return {};
        while (q.next()) {
            QStringList fields{QString::fromLatin1(table)};
            for (int i = 0; i < q.record().count(); ++i) {
                const QVariant v = q.value(i);
                fields << (v.typeId() == QMetaType::QByteArray ? QString::fromLatin1(v.toByteArray().toHex()) : v.toString());
            }
            rows << fields.join(QLatin1Char('|'));
        }
    }
    return rows;
}

QString oldDbIn(const QString &folder)
{
    return QDir(folder).filePath(QStringLiteral("synbench.db"));
}

QString newDbIn(const QString &folder)
{
    return QDir(folder).filePath(QStringLiteral("syntheon.db"));
}

QString tmpOf(const QString &db)
{
    return db + QStringLiteral(".migrating");
}

// Damages one table of an otherwise readable database: the header and `runs` still work.
void damageTable(const QString &path, const QString &table)
{
    int page = 0, pageSize = 0;
    {
        Database::ScopedConnection c(path, QStringLiteral("find"));
        QSqlQuery q(c.db());
        QVERIFY(q.exec(QStringLiteral("SELECT rootpage FROM sqlite_schema WHERE name = '%1'").arg(table)) && q.next());
        page = q.value(0).toInt();
        QVERIFY(q.exec(QStringLiteral("PRAGMA page_size")) && q.next());
        pageSize = q.value(0).toInt();
    }
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.seek(qint64(page - 1) * pageSize));
    f.write(QByteArray(pageSize, char(0xFF)));
}

} // namespace

Q_DECLARE_METATYPE(legacy::Stage)

class TestLegacyMigration : public QObject
{
    Q_OBJECT

    QTemporaryDir m_runtime;

private slots:
    void initTestCase()
    {
        QVERIFY(m_runtime.isValid());
#ifdef Q_OS_UNIX
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(m_runtime.path()));
#endif
    }

    void init()
    {
        // Test-mode locations survive between test functions: start each one clean.
        // Never without test mode: this is a delete of the old default folder.
        QVERIFY(legacy::defaultDataDir().contains(QLatin1StringView("qttest"), Qt::CaseInsensitive));
        QVERIFY(QDir(legacy::defaultDataDir()).removeRecursively());
        QVERIFY(Storage::defaultDataDir().contains(QLatin1StringView("qttest"), Qt::CaseInsensitive));
        QVERIFY(QDir(Storage::defaultDataDir()).removeRecursively());
        QVERIFY(QDir(legacyLockDir()).removeRecursively());
    }

    void legacyPathsPointAtTheOldNames()
    {
        QVERIFY(legacy::defaultDataDir().endsWith(QLatin1StringView("/SynBench")));
        QVERIFY(legacy::settingsFile().endsWith(QLatin1StringView("/SynBench/settings.ini")));
    }

    void legacyLockPathNeverCreatesTheFolder()
    {
        QTemporaryDir history;
        QVERIFY(legacy::lockFilePath(history.path()).isEmpty());
        QVERIFY(!QFileInfo::exists(legacyLockDir()));
        QVERIFY(!QFileInfo::exists(legacy::defaultDataDir()));

        QVERIFY(QDir().mkpath(legacyLockDir()));
        const QString expected = QStringLiteral("synbench-%1.lock").arg(instance::key(history.path()));
        QCOMPARE(legacy::lockFilePath(history.path()), QDir(legacyLockDir()).filePath(expected));
    }

    // M13
    void settingsCopied()
    {
        QTemporaryDir old, fresh, custom;
        const QString legacyIni = old.filePath(QStringLiteral("settings.ini"));
        const QString newIni = fresh.filePath(QStringLiteral("Syntheon/settings.ini"));
        writeLegacyIni(legacyIni, custom.path());
        const QByteArray before = readAll(legacyIni);

        const auto s = legacy::migrateSettings(legacyIni, newIni, old.filePath(QStringLiteral("default")));
        QCOMPARE(s.status, legacy::SettingsStatus::Migrated);
        QCOMPARE(allValues(newIni), allValues(legacyIni));
        QCOMPARE(SettingsStore(newIni).dataLocation(), custom.path());  // a custom path is carried over
        QCOMPARE(readAll(newIni), before);
        QCOMPARE(readAll(legacyIni), before);
        QVERIFY(!QFileInfo::exists(newIni + QStringLiteral(".migrating")));
    }

    void existingNewSettingsAreNotTouched()
    {
        QTemporaryDir dir;
        const QString legacyIni = dir.filePath(QStringLiteral("old.ini")), newIni = dir.filePath(QStringLiteral("new.ini"));
        writeLegacyIni(legacyIni, {});
        writeFile(newIni, "[ui]\ntheme=light\n");
        const QByteArray before = readAll(newIni);
        QCOMPARE(legacy::migrateSettings(legacyIni, newIni, {}).status, legacy::SettingsStatus::NothingToDo);
        QCOMPARE(readAll(newIni), before);
    }

    void noOldSettingsNothingHappens()
    {
        QTemporaryDir dir;
        const QString newIni = dir.filePath(QStringLiteral("sub/new.ini"));
        QCOMPARE(legacy::migrateSettings(dir.filePath(QStringLiteral("old.ini")), newIni, {}).status,
                 legacy::SettingsStatus::NothingToDo);
        QVERIFY(!QFileInfo::exists(QFileInfo(newIni).path()));
    }

    void staleSettingsTempFileIsReplaced()
    {
        QTemporaryDir dir;
        const QString legacyIni = dir.filePath(QStringLiteral("old.ini")), newIni = dir.filePath(QStringLiteral("new.ini"));
        writeLegacyIni(legacyIni, {});
        writeFile(newIni + QStringLiteral(".migrating"), "half a file");
        QCOMPARE(legacy::migrateSettings(legacyIni, newIni, {}).status, legacy::SettingsStatus::Migrated);
        QCOMPARE(readAll(newIni), readAll(legacyIni));
        QVERIFY(!QFileInfo::exists(newIni + QStringLiteral(".migrating")));
    }

    // M14
    void oldDefaultLocationIsDropped_data()
    {
        QTest::addColumn<QString>("suffix");
        QTest::newRow("exact") << QString();
        QTest::newRow("trailing slash") << QStringLiteral("/");
    }
    void oldDefaultLocationIsDropped()
    {
        QFETCH(QString, suffix);
        QTemporaryDir dir;
        const QString oldDefault = dir.filePath(QStringLiteral("SynBench"));
        QVERIFY(QDir().mkpath(oldDefault));
        const QString legacyIni = dir.filePath(QStringLiteral("old.ini")), newIni = dir.filePath(QStringLiteral("new.ini"));
        writeLegacyIni(legacyIni, oldDefault + suffix);
        const QByteArray before = readAll(legacyIni);

        QCOMPARE(legacy::migrateSettings(legacyIni, newIni, oldDefault).status, legacy::SettingsStatus::Migrated);
        SettingsStore migrated(newIni);
        QVERIFY(migrated.dataLocation().isEmpty());
        QCOMPARE(migrated.theme(), QStringLiteral("dark"));  // the rest came across
        QCOMPARE(migrated.layout(), QStringLiteral("tabs"));
        QVERIFY(migrated.firstRunDone());
        QCOMPARE(readAll(legacyIni), before);  // removed from the new file only
    }

    // M15
    void windowsPathsCompareCorrectly()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows paths");
#else
        QTemporaryDir dir;
        const QString oldDefault = dir.filePath(QStringLiteral("SynBench"));
        QVERIFY(QDir().mkpath(oldDefault));
        const QString legacyIni = dir.filePath(QStringLiteral("old.ini")), newIni = dir.filePath(QStringLiteral("new.ini"));
        writeLegacyIni(legacyIni, QDir::toNativeSeparators(oldDefault).toUpper());
        QCOMPARE(legacy::migrateSettings(legacyIni, newIni, oldDefault).status, legacy::SettingsStatus::Migrated);
        QVERIFY(SettingsStore(newIni).dataLocation().isEmpty());
#endif
    }

    // M16
    void unusableNewSettingsPathFails()
    {
        QTemporaryDir dir;
        const QString legacyIni = dir.filePath(QStringLiteral("old.ini"));
        writeLegacyIni(legacyIni, {});
        const QByteArray before = readAll(legacyIni);
        const QString blocker = dir.filePath(QStringLiteral("a-file"));
        writeFile(blocker, "not a folder");
        const QString newIni = blocker + QStringLiteral("/settings.ini");

        const auto s = legacy::migrateSettings(legacyIni, newIni, {});
        QCOMPARE(s.status, legacy::SettingsStatus::Failed);
        QVERIFY(!s.error.isEmpty());
        QVERIFY(!QFileInfo::exists(newIni));
        QVERIFY(!QFileInfo::exists(newIni + QStringLiteral(".migrating")));
        QCOMPARE(readAll(blocker), QByteArray("not a folder"));
        QCOMPARE(readAll(legacyIni), before);
    }

    // A read-only old file is still copied, and its mode doesn't come along: the new
    // file has to be one the app can write. Rows: the temp copy is rewritten or not.
    void readOnlyOldSettingsAreCopied_data()
    {
        QTest::addColumn<bool>("dropsLocation");
        QTest::newRow("copied as is") << false;
        QTest::newRow("key removed") << true;
    }
    void readOnlyOldSettingsAreCopied()
    {
        QFETCH(bool, dropsLocation);
        QTemporaryDir dir;
        const QString oldDefault = dir.filePath(QStringLiteral("SynBench"));
        QVERIFY(QDir().mkpath(oldDefault));
        const QString legacyIni = dir.filePath(QStringLiteral("old.ini")), newIni = dir.filePath(QStringLiteral("new.ini"));
        writeLegacyIni(legacyIni, dropsLocation ? oldDefault : QString());
        const QByteArray before = readAll(legacyIni);
        QVERIFY(QFile::setPermissions(legacyIni, kReadOnly));
        const auto writable = qScopeGuard([&] { QFile::setPermissions(legacyIni, QFile::ReadOwner | QFile::WriteOwner); });

        const auto s = legacy::migrateSettings(legacyIni, newIni, oldDefault);
        QVERIFY2(s.status == legacy::SettingsStatus::Migrated, qPrintable(s.error));
        QVERIFY(!QFileInfo::exists(tmpOf(newIni)));
        QVERIFY(QFile::permissions(newIni) & QFile::WriteOwner);
        SettingsStore migrated(newIni);
        QCOMPARE(migrated.theme(), QStringLiteral("dark"));
        migrated.setTheme(QStringLiteral("light"));
        QVERIFY(migrated.sync());
        QCOMPARE(readAll(legacyIni), before);
        QVERIFY(!(QFile::permissions(legacyIni) & QFile::WriteOwner));
    }

    void readOnlyOldHistoryIsCopied()
    {
        QTemporaryDir data, cfg;
        const QString oldDb = oldDbIn(data.path()), newDb = newDbIn(data.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        const QByteArray before = readAll(oldDb);
        QVERIFY(QFile::setPermissions(oldDb, kReadOnly));
        const auto writable = qScopeGuard([&] { QFile::setPermissions(oldDb, QFile::ReadOwner | QFile::WriteOwner); });

        // Killed with the copy in place: the temp file must not be stuck there.
        const auto killed = legacy::adoptHistory(oldDb, newDb, {}, [](legacy::Stage s) { return s != legacy::Stage::AfterCopy; });
        QCOMPARE(killed.status, legacy::AdoptStatus::Failed);
        QVERIFY2(killed.error == QLatin1StringView("Interrupted."), qPrintable(killed.error));
        QVERIFY(QFileInfo::exists(tmpOf(newDb)));

        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(data.path());
        Storage storage(settings);
        const auto r = storage.open();
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(storage.lastAdoption().status, legacy::AdoptStatus::Adopted);
        QVERIFY(!QFileInfo::exists(tmpOf(newDb)));
        QVERIFY(QFile::permissions(newDb) & QFile::WriteOwner);
        QVERIFY(RunRepository(storage.connection()).saveRun(sampleRun(2000.5)));  // the copy is a working history
        QCOMPARE(RunRepository(storage.connection()).runCount(), 4);
        QCOMPARE(readAll(oldDb), before);
        QVERIFY(!(QFile::permissions(oldDb) & QFile::WriteOwner));
        QVERIFY(!QFileInfo::exists(oldDb + QStringLiteral("-wal")));
        QVERIFY(!QFileInfo::exists(oldDb + QStringLiteral("-shm")));
    }

    // Runs only in the WAL of a file that can't be written: they can't be folded in,
    // and a copy without them must not pass for the history.
    void readOnlyOldHistoryWithAWalFails()
    {
        QTemporaryDir live, old, fresh;
        const QString liveDb = oldDbIn(live.path()), oldDb = oldDbIn(old.path()), newDb = newDbIn(fresh.path());
        QCOMPARE(makeHistory(liveDb, 1).size(), 1);
        {
            Database::ScopedConnection c(liveDb, QStringLiteral("live"));
            QVERIFY(QSqlQuery(c.db()).exec(QStringLiteral("PRAGMA wal_autocheckpoint = 0")));
            QVERIFY(RunRepository(c.db()).saveRun(sampleRun(1234.5)));
            QVERIFY(QFile::copy(liveDb, oldDb));
            QVERIFY(QFile::copy(liveDb + QStringLiteral("-wal"), oldDb + QStringLiteral("-wal")));
        }
        QVERIFY(QFile::setPermissions(oldDb, kReadOnly));
        const auto writable = qScopeGuard([&] { QFile::setPermissions(oldDb, QFile::ReadOwner | QFile::WriteOwner); });
        const auto a = legacy::adoptHistory(oldDb, newDb, {});
        QCOMPARE(a.status, legacy::AdoptStatus::Failed);
        QVERIFY(!QFileInfo::exists(newDb));
        QVERIFY(!QFileInfo::exists(tmpOf(newDb)));
    }

    // M1
    void plainCopy()
    {
        QTemporaryDir old, fresh;
        const QString oldDb = oldDbIn(old.path()), newDb = newDbIn(fresh.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        const QByteArray before = readAll(oldDb);
        const QStringList rows = allRows(oldDb);
        QVERIFY(rows.size() >= 12);

        const auto a = legacy::adoptHistory(oldDb, newDb, {});
        QCOMPARE(a.status, legacy::AdoptStatus::Adopted);
        QCOMPARE(a.runCount, 3);
        QCOMPARE(a.legacyFile, QDir::cleanPath(oldDb));
        QCOMPARE(readAll(newDb), before);
        QCOMPARE(readAll(oldDb), before);
        QVERIFY(!QFileInfo::exists(tmpOf(newDb)));
        QCOMPARE(allRows(newDb), rows);
        QCOMPARE(runsIn(oldDb), 3);
    }

    // The case table's first rows, on the real default folders (test mode). A new
    // folder with no history in it counts as not migrated.
    void defaultFolderIsAdopted_data()
    {
        QTest::addColumn<bool>("newFolderExists");
        QTest::newRow("no new folder") << false;
        QTest::newRow("empty new folder") << true;
    }
    void defaultFolderIsAdopted()
    {
        QFETCH(bool, newFolderExists);
        QTemporaryDir cfg;
        QVERIFY(QDir().mkpath(legacy::defaultDataDir()));
        const QString oldDb = oldDbIn(legacy::defaultDataDir());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        const QByteArray before = readAll(oldDb);
        if (newFolderExists)
            QVERIFY(QDir().mkpath(Storage::defaultDataDir()));

        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        Storage storage(settings);
        const auto r = storage.open();
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(storage.dataDir(), Storage::defaultDataDir());
        QCOMPARE(storage.lastAdoption().status, legacy::AdoptStatus::Adopted);
        QCOMPARE(RunRepository(storage.connection()).runCount(), 3);
        QCOMPARE(readAll(oldDb), before);
    }

    // M2
    void uncheckpointedWalArrives()
    {
        QTemporaryDir live, old, fresh;
        const QString liveDb = oldDbIn(live.path()), oldDb = oldDbIn(old.path()), newDb = newDbIn(fresh.path());
        QCOMPARE(makeHistory(liveDb, 1).size(), 1);
        const RunRecord walRun = sampleRun(1234.5);
        {
            Database::ScopedConnection c(liveDb, QStringLiteral("live"));
            QVERIFY(QSqlQuery(c.db()).exec(QStringLiteral("PRAGMA wal_autocheckpoint = 0")));
            QVERIFY(RunRepository(c.db()).saveRun(walRun));
            QVERIFY(QFileInfo(liveDb + QStringLiteral("-wal")).size() > 0);
            // As a crash leaves them: the run is only in the WAL.
            QVERIFY(QFile::copy(liveDb, oldDb));
            QVERIFY(QFile::copy(liveDb + QStringLiteral("-wal"), oldDb + QStringLiteral("-wal")));
        }

        const auto a = legacy::adoptHistory(oldDb, newDb, {});
        QVERIFY2(a.status == legacy::AdoptStatus::Adopted, qPrintable(a.error));
        QCOMPARE(a.runCount, 2);
        Database::ScopedConnection c(newDb, QStringLiteral("read"));
        const auto loaded = RunRepository(c.db()).load(walRun.uuid);
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->overall, walRun.overall);
    }

    // The same with another connection open, so that closing ours doesn't fold the
    // WAL in by itself: only the explicit checkpoint can get the run into the copy.
    void walIsCheckpointedNotJustClosed()
    {
        QTemporaryDir old, fresh;
        const QString oldDb = oldDbIn(old.path()), newDb = newDbIn(fresh.path());
        QCOMPARE(makeHistory(oldDb, 1).size(), 1);
        Database::ScopedConnection other(oldDb, QStringLiteral("other"));
        QVERIFY(QSqlQuery(other.db()).exec(QStringLiteral("PRAGMA wal_autocheckpoint = 0")));
        QVERIFY(RunRepository(other.db()).saveRun(sampleRun(1234.5)));
        QVERIFY(QFileInfo(oldDb + QStringLiteral("-wal")).size() > 0);

        const auto a = legacy::adoptHistory(oldDb, newDb, {});
        QVERIFY2(a.status == legacy::AdoptStatus::Adopted, qPrintable(a.error));
        QCOMPARE(a.runCount, 2);
        QCOMPARE(runsIn(newDb), 2);
    }

    // M3
    void killedAtEachStage_data()
    {
        QTest::addColumn<legacy::Stage>("stage");
        QTest::addColumn<bool>("newExists");
        QTest::addColumn<bool>("tmpExists");
        QTest::newRow("before copy") << legacy::Stage::BeforeCopy << false << false;
        QTest::newRow("after copy") << legacy::Stage::AfterCopy << false << true;
        QTest::newRow("after verify") << legacy::Stage::AfterVerify << false << true;
        QTest::newRow("after rename") << legacy::Stage::AfterRename << true << false;
    }
    void killedAtEachStage()
    {
        QFETCH(legacy::Stage, stage);
        QFETCH(bool, newExists);
        QFETCH(bool, tmpExists);
        QTemporaryDir data, cfg;
        const QString oldDb = oldDbIn(data.path()), newDb = newDbIn(data.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        const QByteArray before = readAll(oldDb);

        const auto killed = legacy::adoptHistory(oldDb, newDb, {}, [&](legacy::Stage s) { return s != stage; });
        QCOMPARE(killed.status, legacy::AdoptStatus::Failed);
        QCOMPARE(QFileInfo::exists(newDb), newExists);
        QCOMPARE(QFileInfo::exists(tmpOf(newDb)), tmpExists);

        // The next launch.
        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(data.path());
        Storage storage(settings);
        const auto r = storage.open();
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(storage.lastAdoption().status, newExists ? legacy::AdoptStatus::NothingToDo : legacy::AdoptStatus::Adopted);
        QCOMPARE(RunRepository(storage.connection()).runCount(), 3);
        QVERIFY(!QFileInfo::exists(tmpOf(newDb)));
        QCOMPARE(readAll(oldDb), before);
    }

    // M4
    void garbageTempIsReplaced()
    {
        QTemporaryDir old, fresh;
        const QString oldDb = oldDbIn(old.path()), newDb = newDbIn(fresh.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        writeFile(tmpOf(newDb), "half a file");
        writeFile(tmpOf(newDb) + QStringLiteral("-wal"), "not a wal");

        const auto a = legacy::adoptHistory(oldDb, newDb, {});
        QVERIFY2(a.status == legacy::AdoptStatus::Adopted, qPrintable(a.error));
        QCOMPARE(readAll(newDb), readAll(oldDb));
        QVERIFY(!QFileInfo::exists(tmpOf(newDb)));
        QVERIFY(!QFileInfo::exists(tmpOf(newDb) + QStringLiteral("-wal")));
        QCOMPARE(runsIn(newDb), 3);
    }

    // M5
    void bothExistNewOneWins()
    {
        QTemporaryDir data, cfg;
        const QString oldDb = oldDbIn(data.path()), newDb = newDbIn(data.path());
        QCOMPARE(makeHistory(oldDb, 5).size(), 5);
        QCOMPARE(makeHistory(newDb, 2).size(), 2);
        const QByteArray oldBefore = readAll(oldDb), newBefore = readAll(newDb);

        QCOMPARE(legacy::adoptHistory(oldDb, newDb, {}).status, legacy::AdoptStatus::NothingToDo);
        QCOMPARE(readAll(oldDb), oldBefore);
        QCOMPARE(readAll(newDb), newBefore);

        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(data.path());
        {
            Storage storage(settings);
            QVERIFY(storage.open().ok());
            QCOMPARE(storage.lastAdoption().status, legacy::AdoptStatus::NothingToDo);
            QCOMPARE(RunRepository(storage.connection()).runCount(), 2);
        }
        QCOMPARE(readAll(oldDb), oldBefore);
        QCOMPARE(readAll(newDb), newBefore);
    }

    // M6
    void clearedHistoryStaysCleared()
    {
        QTemporaryDir data, cfg;
        const QString oldDb = oldDbIn(data.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(data.path());
        {
            Storage storage(settings);
            QVERIFY(storage.open().ok());
            QCOMPARE(RunRepository(storage.connection()).runCount(), 3);
            QVERIFY(storage.clearHistory());
        }
        Storage storage(settings);
        QVERIFY(storage.open().ok());
        QCOMPARE(RunRepository(storage.connection()).runCount(), 0);
        QCOMPARE(runsIn(oldDb), 3);  // still on disk, not brought back
    }

    // M7
    void noOldDataMeansAFreshHistory()
    {
        QTemporaryDir data, cfg;
        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(data.path());
        Storage storage(settings);
        const auto r = storage.open();
        QCOMPARE(r.status, Storage::OpenStatus::Ok);
        QCOMPARE(storage.lastAdoption().status, legacy::AdoptStatus::NothingToDo);
        QCOMPARE(RunRepository(storage.connection()).runCount(), 0);
        QVERIFY(!QFileInfo::exists(oldDbIn(data.path())));
        QVERIFY(!QFileInfo::exists(legacy::defaultDataDir()));
    }

    // M8: the second open is the point. An empty new file here would hide the old runs for good.
    void garbageOldFileFailsAndCreatesNothing()
    {
        QTemporaryDir data, cfg;
        const QString oldDb = oldDbIn(data.path()), newDb = newDbIn(data.path());
        const QByteArray garbage(8192, 'x');
        writeFile(oldDb, garbage);
        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(data.path());
        for (int attempt = 0; attempt < 2; ++attempt) {
            Storage storage(settings);
            const auto r = storage.open();
            QCOMPARE(r.status, Storage::OpenStatus::Failed);
            QVERIFY2(r.error.contains(QLatin1StringView("were not changed")), qPrintable(r.error));
            QVERIFY(!storage.isOpen());
            QVERIFY(!QFileInfo::exists(newDb));
            QVERIFY(!QFileInfo::exists(tmpOf(newDb)));
            QCOMPARE(readAll(oldDb), garbage);
        }
    }

    void oldFileWithoutRunsTableFails()
    {
        QTemporaryDir old, fresh;
        const QString oldDb = oldDbIn(old.path()), newDb = newDbIn(fresh.path());
        {
            Database::ScopedConnection c(oldDb, QStringLiteral("t"));
            QVERIFY(QSqlQuery(c.db()).exec(QStringLiteral("CREATE TABLE something_else (id INTEGER)")));
        }
        const QByteArray before = readAll(oldDb);
        QCOMPARE(legacy::adoptHistory(oldDb, newDb, {}).status, legacy::AdoptStatus::Failed);
        QVERIFY(!QFileInfo::exists(newDb));
        QCOMPARE(readAll(oldDb), before);
    }

    // Verify failure: a copy that isn't the original never gets the final name.
    void badCopyNeverBecomesTheNewFile_data()
    {
        QTest::addColumn<bool>("flipByte");  // else trailing bytes: only the hash can see those
        QTest::newRow("flipped byte") << true;
        QTest::newRow("trailing bytes") << false;
    }
    void badCopyNeverBecomesTheNewFile()
    {
        QFETCH(bool, flipByte);
        QTemporaryDir old, fresh;
        const QString oldDb = oldDbIn(old.path()), newDb = newDbIn(fresh.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        const QByteArray before = readAll(oldDb);

        const auto a = legacy::adoptHistory(oldDb, newDb, {}, [&](legacy::Stage s) {
            if (s == legacy::Stage::AfterCopy) {
                QFile f(tmpOf(newDb));
                if (!f.open(QIODevice::ReadWrite))
                    return false;
                if (flipByte) {
                    f.seek(f.size() / 2);
                    const char c = f.peek(1).at(0);
                    f.write(QByteArray(1, char(c ^ 0x55)));
                } else {
                    f.seek(f.size());
                    f.write("extra");
                }
            }
            return true;
        });
        QCOMPARE(a.status, legacy::AdoptStatus::Failed);
        QVERIFY2(a.error.contains(QLatin1StringView("were not changed")), qPrintable(a.error));
        QVERIFY(!QFileInfo::exists(newDb));
        QVERIFY(!QFileInfo::exists(tmpOf(newDb)));
        QCOMPARE(readAll(oldDb), before);

        // Nothing sticks: the next attempt copies normally.
        QCOMPARE(legacy::adoptHistory(oldDb, newDb, {}).status, legacy::AdoptStatus::Adopted);
    }

    // A damaged old file that still opens and counts: only the integrity check sees it.
    void damagedOldFileFailsItsIntegrityCheck()
    {
        QTemporaryDir old, fresh;
        const QString oldDb = oldDbIn(old.path()), newDb = newDbIn(fresh.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        damageTable(oldDb, QStringLiteral("metrics"));
        QCOMPARE(runsIn(oldDb), 3);
        const QByteArray before = readAll(oldDb);

        const auto a = legacy::adoptHistory(oldDb, newDb, {});
        QCOMPARE(a.status, legacy::AdoptStatus::Failed);
        QVERIFY2(a.error.contains(QLatin1StringView("integrity")), qPrintable(a.error));
        QVERIFY(!QFileInfo::exists(newDb));
        QVERIFY(!QFileInfo::exists(tmpOf(newDb)));
        QCOMPARE(readAll(oldDb), before);
    }

#ifdef Q_OS_UNIX
    // The disk-full row: the copy itself fails. (Root ignores the permission.)
    void failedCopyLeavesNothing()
    {
        if (geteuid() == 0)
            QSKIP("root can write anywhere");
        QTemporaryDir old, fresh;
        const QString oldDb = oldDbIn(old.path()), newDb = newDbIn(fresh.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        QVERIFY(QFile::setPermissions(fresh.path(), QFile::ReadOwner | QFile::ExeOwner));
        const auto a = legacy::adoptHistory(oldDb, newDb, {});
        QVERIFY(QFile::setPermissions(fresh.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QCOMPARE(a.status, legacy::AdoptStatus::Failed);
        QVERIFY(!QFileInfo::exists(newDb));
        QVERIFY(!QFileInfo::exists(tmpOf(newDb)));
        QCOMPARE(runsIn(oldDb), 3);
    }
#endif

    // M9
    void newerOldFileIsCopiedThenRefused()
    {
        QTemporaryDir data, cfg;
        const QString oldDb = oldDbIn(data.path()), newDb = newDbIn(data.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        {
            Database::ScopedConnection c(oldDb, QStringLiteral("t"));
            QVERIFY(QSqlQuery(c.db()).exec(QStringLiteral("PRAGMA user_version = 99")));
        }
        const QByteArray before = readAll(oldDb);
        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(data.path());
        Storage storage(settings);
        QCOMPARE(storage.open().status, Storage::OpenStatus::NewerVersion);
        QVERIFY(!storage.isOpen());
        QCOMPARE(readAll(oldDb), before);
        QCOMPARE(readAll(newDb), before);
    }

    // M10
    void runningOldBuildBlocksTheCopy()
    {
        QTemporaryDir data, cfg;
        const QString oldDb = oldDbIn(data.path()), newDb = newDbIn(data.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        QVERIFY(QDir().mkpath(legacyLockDir()));
        const QString lockPath = legacy::lockFilePath(data.path());
        QVERIFY(!lockPath.isEmpty());
        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(data.path());
        Storage storage(settings);
        {
            QLockFile oldBuild(lockPath);
            QVERIFY(oldBuild.tryLock(0));
            const auto r = storage.open();
            QCOMPARE(r.status, Storage::OpenStatus::LegacyInUse);
            QVERIFY(!storage.isOpen());
            QVERIFY(!QFileInfo::exists(newDb));
            QVERIFY(!QFileInfo::exists(tmpOf(newDb)));
        }
        const auto r = storage.open();
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(storage.lastAdoption().status, legacy::AdoptStatus::Adopted);
        QCOMPARE(RunRepository(storage.connection()).runCount(), 3);
        QVERIFY(!QFileInfo::exists(lockPath));  // the old lock isn't left behind
    }

    // M11: the path arrives through the migrated settings, the file is copied beside itself.
    void customFolderIsAdoptedInPlace()
    {
        QTemporaryDir data, cfg;
        const QString oldDb = oldDbIn(data.path()), newDb = newDbIn(data.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        const QByteArray before = readAll(oldDb);
        const QString legacyIni = cfg.filePath(QStringLiteral("old.ini")), newIni = cfg.filePath(QStringLiteral("new.ini"));
        writeLegacyIni(legacyIni, data.path());
        QCOMPARE(legacy::migrateSettings(legacyIni, newIni, legacy::defaultDataDir()).status, legacy::SettingsStatus::Migrated);

        SettingsStore settings(newIni);
        Storage storage(settings);
        const auto r = storage.open();
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(storage.dataDir(), QDir::cleanPath(data.path()));
        QCOMPARE(storage.lastAdoption().status, legacy::AdoptStatus::Adopted);
        QCOMPARE(RunRepository(storage.connection()).runCount(), 3);
        QVERIFY(QFileInfo::exists(newDb));
        QCOMPARE(readAll(oldDb), before);
        QVERIFY(!QFileInfo::exists(legacy::defaultDataDir()));
        QVERIFY(!QFileInfo::exists(newDbIn(Storage::defaultDataDir())));
#ifndef Q_OS_WIN  // there the lock folder is inside the default data folder
        QVERIFY(!QFileInfo::exists(Storage::defaultDataDir()));
#endif
    }

    // M12
    void missingCustomFolderWaitsUntilItIsBack()
    {
        QTemporaryDir stash, cfg;
        const QString usb = stash.filePath(QStringLiteral("usb"));
        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(usb);
        Storage storage(settings);
        QCOMPARE(storage.open().status, Storage::OpenStatus::LocationUnavailable);
        QVERIFY(!QFileInfo::exists(usb));

        QVERIFY(QDir().mkpath(usb));
        QCOMPARE(makeHistory(oldDbIn(usb), 3).size(), 3);
        const auto r = storage.open();
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(storage.lastAdoption().status, legacy::AdoptStatus::Adopted);
        QCOMPARE(RunRepository(storage.connection()).runCount(), 3);
    }

    // M17
    void folderHoldingOnlyAnOldHistory()
    {
        QTemporaryDir current, picked, moveTarget, cfg;
        QCOMPARE(makeHistory(oldDbIn(picked.path()), 3).size(), 3);
        QCOMPARE(makeHistory(oldDbIn(moveTarget.path()), 2).size(), 2);
        const QByteArray targetBefore = readAll(oldDbIn(moveTarget.path()));
        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(current.path());
        Storage storage(settings);
        QVERIFY(storage.open().ok());

        // Moving there would hide or overwrite it: it counts as a history.
        QCOMPARE(storage.inspectTarget(moveTarget.path()), Storage::TargetState::HasDatabase);
        QCOMPARE(storage.moveTo(moveTarget.path()).status, Storage::MoveStatus::TargetHasDatabase);
        QCOMPARE(readAll(oldDbIn(moveTarget.path())), targetBefore);
        QVERIFY(!QFileInfo::exists(newDbIn(moveTarget.path())));

        QCOMPARE(storage.inspectTarget(picked.path()), Storage::TargetState::HasDatabase);
        const auto r = storage.switchTo(picked.path());
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(storage.lastAdoption().status, legacy::AdoptStatus::Adopted);
        QCOMPARE(RunRepository(storage.connection()).runCount(), 3);
        QVERIFY(QFileInfo::exists(oldDbIn(picked.path())));
    }

    void failedAdoptionOnSwitchKeepsTheCurrentHistory()
    {
        QTemporaryDir current, picked, cfg;
        writeFile(oldDbIn(picked.path()), QByteArray(8192, 'x'));
        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(current.path());
        Storage storage(settings);
        QVERIFY(storage.open().ok());
        QCOMPARE(storage.switchTo(picked.path()).status, Storage::OpenStatus::Failed);
        QVERIFY(storage.isOpen());
        QCOMPARE(storage.dataDir(), QDir::cleanPath(current.path()));
        QCOMPARE(settings.dataLocation(), current.path());
        QVERIFY(!QFileInfo::exists(newDbIn(picked.path())));
    }

    // M18
    void oldBuildCanStillReadBothFiles()
    {
        QTemporaryDir data, handedBack;
        const QString oldDb = oldDbIn(data.path()), newDb = newDbIn(data.path());
        QCOMPARE(makeHistory(oldDb, 3).size(), 3);
        const QByteArray before = readAll(oldDb);
        QCOMPARE(legacy::adoptHistory(oldDb, newDb, {}).status, legacy::AdoptStatus::Adopted);

        // What an old build does at launch.
        const auto m = Database::migrate(oldDb);
        QVERIFY2(m.ok(), qPrintable(m.error));
        QVERIFY(m.backupPath.isEmpty());
        QCOMPARE(readAll(oldDb), before);
        QCOMPARE(runsIn(oldDb), 3);

        // Handing a Syntheon history back: a run made after the copy, then syntheon.db over synbench.db.
        {
            Database::ScopedConnection c(newDb, QStringLiteral("t"));
            QVERIFY(RunRepository(c.db()).saveRun(sampleRun(2000.5)));
        }
        const QString back = oldDbIn(handedBack.path());
        QVERIFY(QFile::copy(newDb, back));
        const auto b = Database::migrate(back);
        QVERIFY2(b.ok(), qPrintable(b.error));
        QCOMPARE(runsIn(back), 4);
    }

    // M19
    void adoptedRunsLoadWithTheirResults()
    {
        QTemporaryDir old, fresh;
        const QString oldDb = oldDbIn(old.path()), newDb = newDbIn(fresh.path());
        const QVector<RunRecord> saved = makeHistory(oldDb, 3);
        QCOMPARE(saved.size(), 3);
        QCOMPARE(legacy::adoptHistory(oldDb, newDb, {}).status, legacy::AdoptStatus::Adopted);

        Database::ScopedConnection c(newDb, QStringLiteral("read"));
        RunRepository repo(c.db());
        QCOMPARE(repo.listRuns(0, 10).size(), 3);
        // Compare's two sides.
        for (const RunRecord &expected : {saved[0], saved[2]}) {
            const auto loaded = repo.load(expected.uuid);
            QVERIFY(loaded.has_value());
            QCOMPARE(loaded->overall, expected.overall);
            QCOMPARE(loaded->appVersion, expected.appVersion);
            QCOMPARE(loaded->tests.size(), 1);
            const TestResult &want = expected.tests.first(), &got = loaded->tests.first();
            QCOMPARE(got.testId, want.testId);
            QCOMPARE(got.testVersion, want.testVersion);
            QCOMPARE(got.formulaVersion, want.formulaVersion);
            QCOMPARE(got.score, want.score);
            QCOMPARE(got.metrics.size(), want.metrics.size());
            QCOMPARE(got.metrics.first().key, want.metrics.first().key);
            QCOMPARE(got.series.first().points, want.series.first().points);
        }
    }
};

QTEST_GUILESS_MAIN(TestLegacyMigration)
#include "tst_legacy_migration.moc"
