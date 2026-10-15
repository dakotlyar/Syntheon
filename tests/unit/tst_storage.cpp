// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/database.h"
#include "storage/run_repository.h"
#include "storage/settings_store.h"
#include "storage/storage.h"

#include <QDir>
#include <QFile>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace sb;

namespace {

RunRecord sampleRun()
{
    RunRecord r;
    r.uuid = QUuid::createUuid();
    r.kind = RunKind::Standard;
    r.status = RunStatus::Completed;
    r.startedAt = QDateTime(QDate(2026, 10, 15), QTime(14, 2, 3, 456), QTimeZone::UTC);
    r.finishedAt = r.startedAt.addSecs(180);
    r.appVersion = QStringLiteral("2026.11.0");
    r.appBuild = QStringLiteral("linux-x64 abc1234");
    r.system.machineLabel = QStringLiteral("Tiamat");
    r.system.data.insert(QStringLiteral("cpu"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Ryzen")}});
    r.overall = 1694.5;

    TestResult cpu;
    cpu.testId = QStringLiteral("cpu");
    cpu.testVersion = 1;
    cpu.formulaVersion = 2;
    cpu.status = Status::Ok;
    cpu.score = 1284;
    cpu.options = {{QStringLiteral("profile"), QStringLiteral("mixed")}, {QStringLiteral("single"), true},
                   {QStringLiteral("duration"), 30.5}};
    cpu.startedAt = r.startedAt.addMSecs(10);
    cpu.durationMs = 120034;
    cpu.subResults = {{QStringLiteral("int_fp"), Status::Ok, {}, 640.0},
                      {QStringLiteral("simd"), Status::NotAvailable, QStringLiteral("No AVX-512"), std::nullopt}};
    cpu.metrics = {
        {QStringLiteral("int_fp.ops"), QStringLiteral("int_fp"), QStringLiteral("Integer ops"), 152400.0,
         QStringLiteral("MOps/s"), Direction::HigherIsBetter, true},
        {QStringLiteral("simd.path"), {}, QStringLiteral("SIMD path"), QStringLiteral("AVX2"), QString(),
         Direction::Neutral, false},
        {QStringLiteral("latency"), {}, QStringLiteral("Latency"), 0.000123, QStringLiteral("s"),
         Direction::LowerIsBetter, false},
    };
    cpu.series = {{QStringLiteral("curve"), QStringLiteral("Curve"), QStringLiteral("Size"), QStringLiteral("KiB"),
                   QStringLiteral("ns"), {{4, 1.25}, {8, -2.5}, {1e9, 3.141592653589793}}}};

    TestResult npu = TestResult::notAvailable(QStringLiteral("No supported NPU found"));
    npu.testId = QStringLiteral("npu");

    TestResult gpu = TestResult::withStatus(Status::NotRun);
    gpu.testId = QStringLiteral("gpu");

    r.tests = {cpu, npu, gpu};
    return r;
}

int userVersion(const QString &path)
{
    Database::ScopedConnection c(path, QStringLiteral("t"));
    QSqlQuery q(c.db());
    q.exec(QStringLiteral("PRAGMA user_version"));
    q.next();
    return q.value(0).toInt();
}

int rows(QSqlDatabase db, const QString &table)
{
    QSqlQuery q(db);
    q.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table));
    q.next();
    return q.value(0).toInt();
}

#ifdef Q_OS_UNIX
void setWritable(const QString &dir, const QString &file, bool writable)
{
    const auto ro = QFile::ReadOwner | QFile::ExeOwner;
    QFile::setPermissions(dir, writable ? ro | QFile::WriteOwner : ro);
    QFile::setPermissions(file, writable ? QFile::ReadOwner | QFile::WriteOwner : QFile::ReadOwner);
}
#endif

void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

} // namespace

class TestStorage : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void splitScript()
    {
        const QStringList s = Database::splitSqlScript(QStringLiteral(
            "-- comment; with semicolon\nCREATE TABLE a (x TEXT DEFAULT 'a;b''c'); /* block; */ INSERT INTO a "
            "VALUES ('-- not a comment');\n\n  ;"));
        QCOMPARE(s.size(), 2);
        QVERIFY(s[0].startsWith(QLatin1StringView("CREATE TABLE a")));
        QVERIFY(s[0].contains(QLatin1StringView("'a;b''c'")));
        QVERIFY(s[1].contains(QLatin1StringView("'-- not a comment'")));
    }

    // F8: triggers and the other identifier quotes.
    void splitScriptTrigger()
    {
        const QStringList s = Database::splitSqlScript(QStringLiteral(
            "CREATE TABLE a (x INTEGER, y TEXT);\n"
            "CREATE TEMP TRIGGER t1 AFTER INSERT ON a WHEN CASE WHEN new.x > 0 THEN 1 ELSE 0 END\n"
            "BEGIN\n"
            "  UPDATE a SET y = CASE new.x WHEN 1 THEN 'one;' ELSE 'many' END WHERE rowid = new.rowid;\n"
            "  DELETE FROM a WHERE x < 0; -- end; not here\n"
            "END;\n"
            "create trigger t2 before delete on a begin select raise(abort, 'no;'); end;\n"
            "BEGIN TRANSACTION; END;"));
        QCOMPARE(s.size(), 5);
        QVERIFY(s[1].startsWith(QLatin1StringView("CREATE TEMP TRIGGER t1")));
        QVERIFY(s[1].endsWith(QLatin1StringView("END")));
        QVERIFY(s[1].contains(QLatin1StringView("DELETE FROM a WHERE x < 0;")));
        QVERIFY(s[2].startsWith(QLatin1StringView("create trigger t2")));
        QVERIFY(s[2].endsWith(QLatin1StringView("end")));
        // Outside a trigger, BEGIN/END are ordinary statements.
        QCOMPARE(s[3], QStringLiteral("BEGIN TRANSACTION"));
        QCOMPARE(s[4], QStringLiteral("END"));
    }

    void splitScriptQuotedIdentifiers()
    {
        const QStringList s = Database::splitSqlScript(QStringLiteral(
            "CREATE TABLE [we;ird -- name] (`col;``x` TEXT, \"q;\"\"\" INTEGER); SELECT 1"));
        QCOMPARE(s.size(), 2);
        QCOMPARE(s[0], QStringLiteral("CREATE TABLE [we;ird -- name] (`col;``x` TEXT, \"q;\"\"\" INTEGER)"));
        QCOMPARE(s[1], QStringLiteral("SELECT 1"));
    }

    void migrationWithTrigger()
    {
        QTemporaryDir dir, scripts;
        writeFile(scripts.filePath(QStringLiteral("0001_init.sql")),
                  "CREATE TABLE [a b] (x INTEGER, `n;` INTEGER DEFAULT 0);\n"
                  "CREATE TRIGGER bump AFTER INSERT ON [a b] BEGIN\n"
                  "  UPDATE [a b] SET `n;` = new.x * 2 WHERE rowid = new.rowid;\n"
                  "END;\n");
        const QString path = dir.filePath(QStringLiteral("synbench.db"));
        const auto m = Database::migrate(path, scripts.path());
        QVERIFY2(m.ok(), qPrintable(m.error));
        Database::ScopedConnection c(path, QStringLiteral("t"));
        QSqlQuery q(c.db());
        QVERIFY(q.exec(QStringLiteral("INSERT INTO [a b] (x) VALUES (21)")));
        QVERIFY(q.exec(QStringLiteral("SELECT `n;` FROM [a b]")) && q.next());
        QCOMPARE(q.value(0).toInt(), 42);
    }

    void embeddedSchemaVersion() { QCOMPARE(Database::schemaVersion(), 1); }

    void migrateFromEmpty()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("synbench.db"));
        const auto m = Database::migrate(path);
        QVERIFY2(m.ok(), qPrintable(m.error));
        QCOMPARE(m.fromVersion, 0);
        QCOMPARE(m.toVersion, 1);
        QVERIFY(m.backupPath.isEmpty());  // nothing to back up for a new file
        QCOMPARE(userVersion(path), 1);

        Database::ScopedConnection c(path, QStringLiteral("t"));
        QSqlQuery q(c.db());
        QVERIFY(q.exec(QStringLiteral("PRAGMA journal_mode")) && q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("wal"));
        QVERIFY(q.exec(QStringLiteral("PRAGMA foreign_keys")) && q.next());
        QCOMPARE(q.value(0).toInt(), 1);
        for (const char *table :
             {"runs", "test_results", "sub_results", "metrics", "series", "checkpoints", "run_log", "scratch_paths"})
            QCOMPARE(rows(c.db(), QString::fromLatin1(table)), 0);

        // Opening again is a no-op.
        const auto again = Database::migrate(path);
        QVERIFY(again.ok());
        QCOMPARE(again.fromVersion, 1);
        QVERIFY(again.backupPath.isEmpty());
    }

    void refuseNewerVersion()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("synbench.db"));
        {
            Database::ScopedConnection c(path, QStringLiteral("t"));
            QSqlQuery(c.db()).exec(QStringLiteral("PRAGMA user_version = 99"));
        }
        const auto m = Database::migrate(path);
        QCOMPARE(m.status, Database::MigrateResult::Status::NewerVersion);
        QCOMPARE(userVersion(path), 99);
        QVERIFY(!QFile::exists(path + QStringLiteral(".bak-v99")));
        Database::ScopedConnection c(path, QStringLiteral("t"));
        QSqlQuery q(c.db());
        QVERIFY(q.exec(QStringLiteral("PRAGMA journal_mode")) && q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("delete"));  // untouched
    }

    void upgradeMakesBackupAndAppliesInOrder()
    {
        QTemporaryDir dir, scripts;
        writeFile(scripts.filePath(QStringLiteral("0001_init.sql")), "CREATE TABLE a (x INTEGER);");
        const QString path = dir.filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(path, scripts.path()).ok());
        {
            Database::ScopedConnection c(path, QStringLiteral("t"));
            QSqlQuery(c.db()).exec(QStringLiteral("INSERT INTO a VALUES (7)"));
        }

        writeFile(scripts.filePath(QStringLiteral("0002_more.sql")),
                  "ALTER TABLE a ADD COLUMN y TEXT; UPDATE a SET y = 'seven';");
        writeFile(scripts.filePath(QStringLiteral("0003_more.sql")), "CREATE TABLE b (z INTEGER);");
        const auto m = Database::migrate(path, scripts.path());
        QVERIFY2(m.ok(), qPrintable(m.error));
        QCOMPARE(m.fromVersion, 1);
        QCOMPARE(m.toVersion, 3);
        QCOMPARE(m.backupPath, path + QStringLiteral(".bak-v1"));
        QCOMPARE(userVersion(path), 3);
        QCOMPARE(userVersion(m.backupPath), 1);

        Database::ScopedConnection c(path, QStringLiteral("t"));
        QSqlQuery q(c.db());
        QVERIFY(q.exec(QStringLiteral("SELECT y FROM a")) && q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("seven"));
        Database::ScopedConnection b(m.backupPath, QStringLiteral("t"));
        QCOMPARE(rows(b.db(), QStringLiteral("a")), 1);
    }

    void failedMigrationRollsBack()
    {
        QTemporaryDir dir, scripts;
        writeFile(scripts.filePath(QStringLiteral("0001_init.sql")), "CREATE TABLE a (x INTEGER);");
        const QString path = dir.filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(path, scripts.path()).ok());
        writeFile(scripts.filePath(QStringLiteral("0002_bad.sql")),
                  "CREATE TABLE half (x INTEGER); THIS IS NOT SQL;");
        const auto m = Database::migrate(path, scripts.path());
        QCOMPARE(m.status, Database::MigrateResult::Status::Failed);
        QVERIFY(m.error.contains(QLatin1StringView("Migration 2")));
        QCOMPARE(userVersion(path), 1);
        Database::ScopedConnection c(path, QStringLiteral("t"));
        QSqlQuery q(c.db());
        QVERIFY(!q.exec(QStringLiteral("SELECT * FROM half")));  // rolled back
    }

    void missingMigrationFails()
    {
        QTemporaryDir dir, scripts;
        writeFile(scripts.filePath(QStringLiteral("0002_gap.sql")), "CREATE TABLE a (x INTEGER);");
        const auto m = Database::migrate(dir.filePath(QStringLiteral("synbench.db")), scripts.path());
        QCOMPARE(m.status, Database::MigrateResult::Status::Failed);
    }

    void runRecordRoundTrip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(path).ok());
        Database::ScopedConnection c(path, QStringLiteral("t"));
        RunRepository repo(c.db());

        const RunRecord r = sampleRun();
        const auto id = repo.saveRun(r);
        QVERIFY2(id, qPrintable(repo.lastError()));
        const auto loaded = repo.load(r.uuid);
        QVERIFY(loaded);
        QCOMPARE(loaded->tests.size(), 3);
        QCOMPARE(loaded->tests[0].metrics, r.tests[0].metrics);
        QCOMPARE(loaded->tests[0].series, r.tests[0].series);
        QCOMPARE(loaded->tests[0].subResults, r.tests[0].subResults);
        QCOMPARE(loaded->tests[0].options, r.tests[0].options);
        QCOMPARE(loaded->system, r.system);
        QVERIFY(*loaded == r);
        QCOMPARE(repo.runId(r.uuid), id);
        QVERIFY(!repo.load(QUuid::createUuid()));

        // Exact series bits survive (packed little-endian float64).
        QCOMPARE(RunRepository::unpackPoints(RunRepository::packPoints(r.tests[0].series[0].points)),
                 r.tests[0].series[0].points);
        QCOMPARE(RunRepository::packPoints({{1.0, 2.0}}).size(), 16);

        // The machine label is copied out for the History list.
        QSqlQuery q(c.db());
        QVERIFY(q.exec(QStringLiteral("SELECT machine_label, started_at FROM runs")) && q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("Tiamat"));
        QCOMPARE(q.value(1).toString(), QStringLiteral("2026-10-15T14:02:03.456Z"));
    }

    void progressiveWrites()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(path).ok());
        Database::ScopedConnection c(path, QStringLiteral("t"));
        RunRepository repo(c.db());

        RunRecord r = sampleRun();
        r.status = RunStatus::Running;
        r.finishedAt = {};
        r.overall.reset();
        const auto id = repo.insertRun(r);
        QVERIFY(id);
        QVERIFY(repo.insertTestResult(*id, 0, r.tests[0]));

        auto loaded = repo.load(r.uuid);
        QCOMPARE(loaded->status, RunStatus::Running);
        QCOMPARE(loaded->tests.size(), 1);
        QVERIFY(!loaded->finishedAt.isValid());

        // A test result row is all-or-nothing: a duplicate test id fails and
        // leaves no partial rows behind.
        QVERIFY(!repo.insertTestResult(*id, 1, r.tests[0]));
        QCOMPARE(rows(c.db(), QStringLiteral("test_results")), 1);
        QCOMPARE(rows(c.db(), QStringLiteral("metrics")), 3);

        const QDateTime done = r.startedAt.addSecs(60);
        QVERIFY(repo.finishRun(*id, RunStatus::Stopped, done, 1284.0));
        loaded = repo.load(r.uuid);
        QCOMPARE(loaded->status, RunStatus::Stopped);
        QCOMPARE(loaded->finishedAt, done);
        QCOMPARE(loaded->overall, std::optional<double>(1284.0));
        QVERIFY(!repo.finishRun(*id + 100, RunStatus::Completed, done, std::nullopt));
    }

    // F3: the repository never lets a non-finite value roll back a test row.
    void nonFiniteMetricsStoredAsText()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(path).ok());
        Database::ScopedConnection c(path, QStringLiteral("t"));
        RunRepository repo(c.db());
        RunRecord r = sampleRun();
        r.tests.resize(1);
        r.tests[0].metrics = {
            {QStringLiteral("a"), {}, QStringLiteral("A"), qQNaN(), {}, Direction::Neutral, false},
            {QStringLiteral("b"), {}, QStringLiteral("B"), qInf(), {}, Direction::Neutral, false},
            {QStringLiteral("c"), {}, QStringLiteral("C"), -qInf(), {}, Direction::Neutral, false},
            {QStringLiteral("d"), {}, QString(), 1.5, {}, Direction::Neutral, false},  // null label
        };
        QVERIFY2(repo.saveRun(r), qPrintable(repo.lastError()));
        const auto loaded = repo.load(r.uuid);
        QVERIFY(loaded);
        const auto &m = loaded->tests[0].metrics;
        QCOMPARE(m.size(), 4);
        QCOMPARE(std::get<QString>(m[0].value), QStringLiteral("nan"));
        QCOMPARE(std::get<QString>(m[1].value), QStringLiteral("inf"));
        QCOMPARE(std::get<QString>(m[2].value), QStringLiteral("-inf"));
        QCOMPARE(std::get<double>(m[3].value), 1.5);
    }

    void runLog()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(path).ok());
        Database::ScopedConnection c(path, QStringLiteral("t"));
        RunRepository repo(c.db());
        const auto id = repo.saveRun(sampleRun());
        QVERIFY(id);

        const QDateTime t0 = QDateTime::currentDateTimeUtc();
        const QVector<LogLine> first = {{t0, LogLevel::Info, {}, QStringLiteral("Run started")},
                                        {t0.addMSecs(5), LogLevel::Warning, QStringLiteral("cpu"),
                                         QStringLiteral("AVX2 path selected (AVX-512 not present)")}};
        const QVector<LogLine> second = {{t0.addMSecs(9), LogLevel::Error, QStringLiteral("gpu"),
                                          QStringLiteral("boom")}};
        QVERIFY(repo.appendLog(*id, first));
        QVERIFY(repo.appendLog(*id, second));
        QVERIFY(repo.appendLog(*id, {}));
        QCOMPARE(repo.loadLog(*id), first + second);
        QVERIFY(repo.loadLog(*id + 1).isEmpty());
    }

    void clearHistoryCascades()
    {
        QTemporaryDir dir;
        SettingsStore settings(dir.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(dir.path());
        Storage storage(settings);
        QVERIFY(storage.open().ok());

        RunRepository repo(storage.connection());
        const auto id = repo.saveRun(sampleRun());
        QVERIFY(id);
        QVERIFY(repo.saveRun(sampleRun()));
        QVERIFY(repo.appendLog(*id, {{QDateTime::currentDateTimeUtc(), LogLevel::Info, {}, QStringLiteral("x")}}));
        QVERIFY(repo.addCheckpoint(*id, QStringLiteral("battery"), 0, QByteArray("\xa0", 1)));
        QVERIFY(repo.addScratchPath(*id, QStringLiteral("/tmp/x")));

        storage.setBusy(true);
        QString error;
        QVERIFY(!storage.clearHistory(&error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(repo.runCount(), 2);

        storage.setBusy(false);
        QVERIFY2(storage.clearHistory(&error), qPrintable(error));
        for (const char *table :
             {"runs", "test_results", "sub_results", "metrics", "series", "checkpoints", "run_log", "scratch_paths"})
            QCOMPARE(rows(storage.connection(), QString::fromLatin1(table)), 0);
    }

    void walReaderDuringWriter()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(path).ok());
        Database::ScopedConnection writer(path, QStringLiteral("w"));
        Database::ScopedConnection reader(path, QStringLiteral("r"));
        RunRepository w(writer.db());
        RunRepository r(reader.db());

        QVERIFY(writer.db().transaction());
        QVERIFY(w.insertRun(sampleRun()));
        // The reader isn't blocked by the open write transaction and doesn't
        // see uncommitted data.
        QCOMPARE(r.runCount(), 0);
        QVERIFY(writer.db().commit());
        QCOMPARE(r.runCount(), 1);
    }

    void settingsDefaultsAndPersistence()
    {
        QTemporaryDir dir;
        const QString ini = dir.filePath(QStringLiteral("settings.ini"));
        {
            SettingsStore s(ini);
            QCOMPARE(s.layout(), QStringLiteral("rail"));
            QCOMPARE(s.theme(), QStringLiteral("system"));
            QVERIFY(s.dataLocation().isEmpty());
            QVERIFY(!s.firstRunDone());
            s.setLayout(QStringLiteral("tabs"));
            s.setTheme(QStringLiteral("dark"));
            s.setDataLocation(QStringLiteral("/mnt/usb/SynBench"));
            s.setFirstRunDone(true);
            QVERIFY(s.sync());
        }
        QVERIFY(QFile::exists(ini));
        SettingsStore s(ini);
        QCOMPARE(s.layout(), QStringLiteral("tabs"));
        QCOMPARE(s.theme(), QStringLiteral("dark"));
        QCOMPARE(s.dataLocation(), QStringLiteral("/mnt/usb/SynBench"));
        QVERIFY(s.firstRunDone());

        s.setTheme(QStringLiteral("purple"));
        QCOMPARE(s.theme(), QStringLiteral("system"));  // unknown values fall back
        s.setDataLocation({});
        QVERIFY(s.dataLocation().isEmpty());
    }

    void defaultSettingsFile()
    {
        const SettingsStore s;
        QVERIFY(s.fileName().endsWith(QLatin1StringView("SynBench/settings.ini")));
    }

    void defaultLocationCreated()
    {
        QTemporaryDir dir;
        SettingsStore settings(dir.filePath(QStringLiteral("settings.ini")));
        Storage storage(settings);
        QVERIFY(Storage::defaultDataDir().endsWith(QLatin1StringView("/SynBench")));
        QDir(Storage::defaultDataDir()).removeRecursively();  // test-mode location
        const auto r = storage.open();
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(storage.dataDir(), Storage::defaultDataDir());
        QVERIFY(QFile::exists(storage.dbPath()));
        QDir(Storage::defaultDataDir()).removeRecursively();
    }

    void missingLocationUnavailable()
    {
        QTemporaryDir dir;
        SettingsStore settings(dir.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(dir.filePath(QStringLiteral("removed-usb-drive")));
        Storage storage(settings);
        const auto r = storage.open();
        QCOMPARE(r.status, Storage::OpenStatus::LocationUnavailable);
        QVERIFY(!storage.isOpen());

        const auto d = storage.useDefaultLocation();
        QVERIFY2(d.ok(), qPrintable(d.error));
        QVERIFY(settings.dataLocation().isEmpty());
        QCOMPARE(storage.dataDir(), Storage::defaultDataDir());
        QDir(Storage::defaultDataDir()).removeRecursively();
    }

    void newerDatabaseAtLaunch()
    {
        QTemporaryDir dir;
        {
            Database::ScopedConnection c(dir.filePath(QStringLiteral("synbench.db")), QStringLiteral("t"));
            QSqlQuery(c.db()).exec(QStringLiteral("PRAGMA user_version = 5"));
        }
        SettingsStore settings(dir.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(dir.path());
        Storage storage(settings);
        QCOMPARE(storage.open().status, Storage::OpenStatus::NewerVersion);
        QVERIFY(!storage.isOpen());
    }

    void moveDataLocation()
    {
        QTemporaryDir oldDir, newDir, cfg;
        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(oldDir.path());
        Storage storage(settings);
        QVERIFY(storage.open().ok());
        const RunRecord r = sampleRun();
        QVERIFY(RunRepository(storage.connection()).saveRun(r));
        QVERIFY(RunRepository(storage.connection()).saveRun(sampleRun()));
        const QString oldPath = storage.dbPath();

        QCOMPARE(storage.inspectTarget(newDir.path()), Storage::TargetState::NoDatabase);
        const auto m = storage.moveTo(newDir.path());
        QCOMPARE(m.status, Storage::MoveStatus::Moved);
        QVERIFY2(m.warning.isEmpty(), qPrintable(m.warning));

        QCOMPARE(storage.dataDir(), QDir::cleanPath(newDir.path()));
        QCOMPARE(settings.dataLocation(), QDir::cleanPath(newDir.path()));
        QVERIFY(!QFile::exists(oldPath));
        QVERIFY(!QFile::exists(oldPath + QStringLiteral("-wal")));
        QVERIFY(storage.isOpen());
        RunRepository repo(storage.connection());
        QCOMPARE(repo.runCount(), 2);
        QVERIFY(*repo.load(r.uuid) == r);

        // The new location survives a restart.
        SettingsStore reread(cfg.filePath(QStringLiteral("settings.ini")));
        QCOMPARE(reread.dataLocation(), QDir::cleanPath(newDir.path()));
    }

    void moveToFolderWithDatabaseOpensNeverMerges()
    {
        QTemporaryDir aDir, bDir, cfg;
        // History B already exists.
        {
            SettingsStore sb(cfg.filePath(QStringLiteral("b.ini")));
            sb.setDataLocation(bDir.path());
            Storage b(sb);
            QVERIFY(b.open().ok());
            QVERIFY(RunRepository(b.connection()).saveRun(sampleRun()));
        }

        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(aDir.path());
        Storage storage(settings);
        QVERIFY(storage.open().ok());
        QVERIFY(RunRepository(storage.connection()).saveRun(sampleRun()));
        QVERIFY(RunRepository(storage.connection()).saveRun(sampleRun()));

        QCOMPARE(storage.inspectTarget(bDir.path()), Storage::TargetState::HasDatabase);
        const auto m = storage.moveTo(bDir.path());
        QCOMPARE(m.status, Storage::MoveStatus::TargetHasDatabase);
        // Nothing changed.
        QCOMPARE(storage.dataDir(), QDir::cleanPath(aDir.path()));
        QCOMPARE(RunRepository(storage.connection()).runCount(), 2);

        // "Open the history found there?" -> Open.
        const auto o = storage.switchTo(bDir.path());
        QVERIFY2(o.ok(), qPrintable(o.error));
        QCOMPARE(storage.dataDir(), QDir::cleanPath(bDir.path()));
        QCOMPARE(settings.dataLocation(), QDir::cleanPath(bDir.path()));
        QCOMPARE(RunRepository(storage.connection()).runCount(), 1);  // B's own history, unmerged
        QVERIFY(QFile::exists(QDir(aDir.path()).filePath(QStringLiteral("synbench.db"))));  // A left in place
    }

    void moveFailureKeepsOldLocation()
    {
        QTemporaryDir oldDir, cfg, parent;
#ifdef Q_OS_UNIX
        if (geteuid() == 0)
            QSKIP("root ignores directory permissions");
        // A read-only target folder.
        const QString readOnly = parent.filePath(QStringLiteral("readonly"));
        QVERIFY(QDir().mkpath(readOnly));
        QVERIFY(QFile::setPermissions(readOnly, QFile::ReadOwner | QFile::ExeOwner));
#else
        // Windows can't make a folder read-only through permission bits; use a
        // folder that doesn't exist instead.
        const QString readOnly = parent.filePath(QStringLiteral("missing"));
#endif

        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(oldDir.path());
        Storage storage(settings);
        QVERIFY(storage.open().ok());
        QVERIFY(RunRepository(storage.connection()).saveRun(sampleRun()));

        QCOMPARE(storage.inspectTarget(readOnly), Storage::TargetState::Unusable);
        const auto m = storage.moveTo(readOnly);
        QCOMPARE(m.status, Storage::MoveStatus::Failed);
        QVERIFY(!m.error.isEmpty());
        QCOMPARE(storage.dataDir(), QDir::cleanPath(oldDir.path()));
        QCOMPARE(settings.dataLocation(), oldDir.path());
        QVERIFY(storage.isOpen());
        QCOMPARE(RunRepository(storage.connection()).runCount(), 1);

        QFile::setPermissions(readOnly, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    // F1: one failed settings write must not make every later sync fail.
    void settingsSyncRecoversAfterFailure()
    {
#ifndef Q_OS_UNIX
        QSKIP("needs POSIX permissions");
#else
        if (geteuid() == 0)
            QSKIP("root ignores permissions");
        QTemporaryDir dir;
        const QString cfg = dir.filePath(QStringLiteral("cfg"));
        QVERIFY(QDir().mkpath(cfg));
        const QString ini = QDir(cfg).filePath(QStringLiteral("settings.ini"));
        SettingsStore s(ini);
        s.setTheme(QStringLiteral("dark"));
        s.setDataLocation(QStringLiteral("/somewhere"));
        QVERIFY(s.sync());

        setWritable(cfg, ini, false);
        s.setLayout(QStringLiteral("tabs"));
        s.setDataLocation({});
        QVERIFY(!s.sync());
        setWritable(cfg, ini, true);
        QVERIFY(s.sync());

        SettingsStore reread(ini);
        QCOMPARE(reread.layout(), QStringLiteral("tabs"));
        QCOMPARE(reread.theme(), QStringLiteral("dark"));
        QVERIFY(reread.dataLocation().isEmpty());  // the removal reached the disk too
#endif
    }

    void moveAndSwitchWorkAfterSettingsFailure()
    {
#ifndef Q_OS_UNIX
        QSKIP("needs POSIX permissions");
#else
        if (geteuid() == 0)
            QSKIP("root ignores permissions");
        QTemporaryDir oldDir, newDir, otherDir, dir;
        const QString cfg = dir.filePath(QStringLiteral("cfg"));
        QVERIFY(QDir().mkpath(cfg));
        const QString ini = QDir(cfg).filePath(QStringLiteral("settings.ini"));
        SettingsStore settings(ini);
        settings.setDataLocation(oldDir.path());
        QVERIFY(settings.sync());
        Storage storage(settings);
        QVERIFY(storage.open().ok());
        QVERIFY(RunRepository(storage.connection()).saveRun(sampleRun()));

        setWritable(cfg, ini, false);
        auto m = storage.moveTo(newDir.path());
        QCOMPARE(m.status, Storage::MoveStatus::Failed);
        QCOMPARE(storage.dataDir(), QDir::cleanPath(oldDir.path()));
        QVERIFY(!QFile::exists(QDir(newDir.path()).filePath(QStringLiteral("synbench.db"))));
        QCOMPARE(RunRepository(storage.connection()).runCount(), 1);

        setWritable(cfg, ini, true);
        m = storage.moveTo(newDir.path());
        QCOMPARE(m.status, Storage::MoveStatus::Moved);
        QCOMPARE(SettingsStore(ini).dataLocation(), QDir::cleanPath(newDir.path()));

        setWritable(cfg, ini, false);
        QVERIFY(!storage.switchTo(otherDir.path()).ok());
        QCOMPARE(storage.dataDir(), QDir::cleanPath(newDir.path()));
        setWritable(cfg, ini, true);
        QVERIFY(storage.switchTo(otherDir.path()).ok());
        QCOMPARE(SettingsStore(ini).dataLocation(), QDir::cleanPath(otherDir.path()));
#endif
    }

    void moveRefusedWhileBusy()
    {
        QTemporaryDir oldDir, newDir, cfg;
        SettingsStore settings(cfg.filePath(QStringLiteral("settings.ini")));
        settings.setDataLocation(oldDir.path());
        Storage storage(settings);
        QVERIFY(storage.open().ok());
        storage.setBusy(true);
        QCOMPARE(storage.moveTo(newDir.path()).status, Storage::MoveStatus::Refused);
        QVERIFY(!storage.switchTo(newDir.path()).ok());
        QCOMPARE(storage.dataDir(), QDir::cleanPath(oldDir.path()));
        QVERIFY(!QFile::exists(QDir(newDir.path()).filePath(QStringLiteral("synbench.db"))));
    }
};

QTEST_GUILESS_MAIN(TestStorage)
#include "tst_storage.moc"
