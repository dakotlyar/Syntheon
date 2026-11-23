// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/controllers/run_controller.h"
#include "app/controllers/settings_controller.h"
#include "app/controllers/startup_controller.h"
#include "app/controllers/storage_controller.h"
#include "storage/database.h"
#include "storage/legacy_migration.h"
#include "tests/common/stub_clock.h"
#include "storage/run_repository.h"

#include <QCborMap>
#include <QCborValue>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace sb;

namespace {

RunRecord run()
{
    RunRecord r;
    r.uuid = QUuid::createUuid();
    r.status = RunStatus::Completed;
    r.startedAt = QDateTime::currentDateTimeUtc();
    return r;
}

} // namespace

class TestControllers : public QObject
{
    Q_OBJECT

private slots:
    void settingsPersistAndValidate()
    {
        QTemporaryDir dir;
        const QString ini = dir.filePath(QStringLiteral("settings.ini"));
        {
            SettingsStore store(ini);
            SettingsController c(store);
            QCOMPARE(c.layout(), QStringLiteral("rail"));
            QCOMPARE(c.theme(), QStringLiteral("system"));
            QVERIFY(!c.firstRunDone());
            QSignalSpy layout(&c, &SettingsController::layoutChanged), theme(&c, &SettingsController::themeChanged);
            c.setLayout(QStringLiteral("tabs"));
            c.setTheme(QStringLiteral("dark"));
            c.setLayout(QStringLiteral("tabs"));          // no change, no signal
            c.setTheme(QStringLiteral("purple"));         // rejected
            c.setLayout(QStringLiteral("sideways"));      // rejected
            QCOMPARE(layout.count(), 1);
            QCOMPARE(theme.count(), 1);
            c.setFirstRunDone(true);
        }
        SettingsStore again(ini);
        SettingsController c(again);
        QCOMPARE(c.layout(), QStringLiteral("tabs"));
        QCOMPARE(c.theme(), QStringLiteral("dark"));
        QVERIFY(c.firstRunDone());
    }

    void systemInfoScreenDataAndEmulationBanner()
    {
        QTemporaryDir dir;
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = dir.filePath(QStringLiteral("a"));
        AppRuntime rt(o);
        AppController &a = rt.app();
        QVERIFY(a.systemRows().isEmpty());  // nothing before the snapshot arrives
        QVERIFY(a.emulationText().isEmpty());

        SystemInfo i;
        i.machine.hostname = QStringLiteral("Seinar");
        i.machine.maker = QStringLiteral("ASUS");
        i.machine.model = QStringLiteral("TUF Gaming A14");
        i.cpu.name = QStringLiteral("Test CPU");
        i.cpu.cores = 8;
        i.cpu.threads = 16;
        i.cpu.maxMhz = 5100;
        i.memory.totalBytes = qint64(32) << 30;
        i.os.name = QStringLiteral("Windows 11");
        i.os.version = QStringLiteral("24H2");
        i.os.arch = QStringLiteral("arm64");
        i.battery = {true, 90.0, 84.6, 94.0, 100.0, true, 212};
        i.apis.vulkanInstanceVersion = QStringLiteral("1.4.3");
        i.apis.d3d12FeatureLevel = QStringLiteral("12_1");
        i.emulated = true;
        i.nativeArch = QStringLiteral("arm64");
        a.setSystemInfoForTest(i);

        const QVariantList rows = a.systemRows();
        QVERIFY(rows.size() >= 8);
        QCOMPARE(rows.first().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Machine"));
        QCOMPARE(rows[1 + 0].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Processor"));
        QVERIFY(rows[1].toMap().value(QStringLiteral("value")).toString().contains(QStringLiteral("8 cores / 16 threads")));
        QVERIFY(a.headerMachine().contains(QStringLiteral("Seinar")));
        QVERIFY(a.headerMachine().contains(QStringLiteral("TUF Gaming A14")));
        QCOMPARE(a.headerDetail(), QStringLiteral("Windows 11 arm64 · on AC power"));
        QVERIFY(a.emulated());
        QVERIFY(a.emulationText().contains(QStringLiteral("emulated")));
        QCOMPARE(a.apiPills().size(), 2);  // DirectX 12 and Vulkan; no D3D11/OpenGL given
        const QStringList battery = a.batteryLines();
        QCOMPARE(battery.first(), QStringLiteral("90.0 Wh design · 84.6 Wh full charge"));
        QVERIFY(battery.last().contains(QStringLiteral("Health 94%")));
        QVERIFY(battery.last().contains(QStringLiteral("212 cycles")));

        i.battery.present = false;
        i.emulated = false;
        a.setSystemInfoForTest(i);
        QCOMPARE(a.batteryLines(), QStringList{QStringLiteral("No battery found")});
        QVERIFY(a.emulationText().isEmpty());
        QVERIFY(a.headerDetail().endsWith(QStringLiteral("no battery")));
    }

    // Startup recovery (design 3): a run left 'running' by a crash is finished
    // off, a one-time notice is built, and the history stays locked only while
    // that happens.
    void startupRecoversCutShortRunsAndNotifiesOnce()
    {
        QTemporaryDir dir;
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = dir.filePath(QStringLiteral("a"));
        AppRuntime rt(o);
        rt.storageController().open();
        StartupController &st = rt.startup();
        auto recoverNow = [&] {
            st.recover();
            st.waitForRecovery();
            QVERIFY(!st.recovering());
        };
        recoverNow();
        QVERIFY(!st.recoveryPending());  // an empty history: nothing to report

        auto seed = [&](const QString &testId, RunKind kind) {
            RunRecord r;
            r.uuid = QUuid::createUuid();
            r.kind = kind;
            r.status = RunStatus::Running;
            r.startedAt = QDateTime::currentDateTimeUtc().addSecs(-7200);
            r.appVersion = QStringLiteral("0.4.3");
            TestResult t = TestResult::withStatus(Status::NotRun, QStringLiteral("Queued"));
            t.testId = testId;
            r.tests = {t};
            RunRepository repo(rt.storage().connection());
            const auto id = repo.saveRun(r);
            [&] { QVERIFY(id); }();
            return std::make_pair(r.uuid, *id);
        };

        const auto cut = seed(QStringLiteral("cpu"), RunKind::Standard);
        recoverNow();
        QVERIFY(st.recoveryPending());
        QVERIFY(st.recoveryNotice().contains(QStringLiteral("was interrupted")));
        QVERIFY(!rt.storageController().locked());
        {
            RunRepository repo(rt.storage().connection());
            QCOMPARE(repo.load(cut.first)->status, RunStatus::Interrupted);
        }
        st.dismissRecoveryNotice();
        QVERIFY(!st.recoveryPending());
        recoverNow();  // nothing left to recover: the notice doesn't come back
        QVERIFY(!st.recoveryPending());

        const auto battery = seed(QStringLiteral("battery"), RunKind::Exclusive);
        {
            RunRepository repo(rt.storage().connection());
            for (int i = 0; i < 3; ++i)
                QVERIFY(repo.addCheckpoint(battery.second, QStringLiteral("battery"), i,
                                           QCborValue(QCborMap{{QStringLiteral("t"), i * 100.0},
                                                               {QStringLiteral("charge"), 100.0 - i * 30}}).toCbor()));
        }
        recoverNow();
        QVERIFY(st.recoveryPending());
        QVERIFY2(st.recoveryNotice().contains(QStringLiteral("recovered: 3 h 20 m")), qPrintable(st.recoveryNotice()));
        {
            RunRepository repo(rt.storage().connection());
            QCOMPARE(repo.load(battery.first)->status, RunStatus::Recovered);
        }
        QCOMPARE(StartupController::hoursMinutes((6 * 60 + 42) * 60000LL), QStringLiteral("6 h 42 m"));
    }

    // AP-5 #1: the notice must survive a normal start (open() triggers the
    // pass; a second pass that finds nothing must not blank it).
    void noticeSurvivesAFullStart()
    {
        QTemporaryDir dir;
        const QString data = dir.filePath(QStringLiteral("data"));
        QDir().mkpath(data);
        {
            QVERIFY(Database::migrate(QDir(data).filePath(QStringLiteral("syntheon.db"))).ok());
            Database::ScopedConnection c(QDir(data).filePath(QStringLiteral("syntheon.db")), QStringLiteral("seed"));
            RunRecord r;
            r.uuid = QUuid::createUuid();
            r.status = RunStatus::Running;
            r.startedAt = QDateTime::currentDateTimeUtc().addSecs(-3600);
            TestResult t = TestResult::withStatus(Status::NotRun, QStringLiteral("Queued"));
            t.testId = QStringLiteral("cpu");
            r.tests = {t};
            QVERIFY(RunRepository(c.db()).saveRun(r));
        }
        AppRuntime::Options o;
        o.noElevation = true;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = data;
        AppRuntime rt(o);
        QVERIFY(rt.claimInstance());
        rt.start();
        QTRY_VERIFY_WITH_TIMEOUT(rt.startup().recoveryPending(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!rt.startup().recovering(), 10000);
        QCoreApplication::processEvents();
        QVERIFY(rt.startup().recoveryPending());
        QVERIFY(rt.startup().recoveryNotice().contains(QStringLiteral("was interrupted")));
        QVERIFY(!rt.storageController().locked());
    }

    // The first launch after the rename says what it did, once.
    void copiedHistoryIsAnnouncedOnce()
    {
        QTemporaryDir dir, other;
        const QString data = dir.filePath(QStringLiteral("data"));
        QDir().mkpath(data);
        const QString oldDb = QDir(data).filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(oldDb).ok());
        {
            Database::ScopedConnection c(oldDb, QStringLiteral("seed"));
            QVERIFY(RunRepository(c.db()).saveRun(run()));
            QVERIFY(RunRepository(c.db()).saveRun(run()));
        }
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = data;
        AppRuntime rt(o);
        rt.storageController().open();
        StartupController &st = rt.startup();
        st.waitForRecovery();
        QCOMPARE(st.recoveryNotice(),
                 QStringLiteral("Your 2 saved runs from SynBench were copied here. The old copy, %1, was left in place; you can delete it.")
                     .arg(QDir::toNativeSeparators(QDir::cleanPath(oldDb))));
        st.dismissRecoveryNotice();

        st.recover();  // every location change runs this again
        st.waitForRecovery();
        QVERIFY(!st.recoveryPending());
        // A folder with a history of its own runs no copy, and lastAdoption() stays as it was.
        QVERIFY(Database::migrate(QDir(other.path()).filePath(QStringLiteral("syntheon.db"))).ok());
        QCOMPARE(rt.storageController().openHistoryIn(other.path()), QString());
        st.waitForRecovery();
        QVERIFY2(!st.recoveryPending(), qPrintable(st.recoveryNotice()));
    }

    void oneCopiedRunReadsRight()
    {
        QTemporaryDir dir;
        const QString oldDb = dir.filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(oldDb).ok());
        {
            Database::ScopedConnection c(oldDb, QStringLiteral("seed"));
            QVERIFY(RunRepository(c.db()).saveRun(run()));
        }
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = dir.path();
        AppRuntime rt(o);
        rt.storageController().open();
        rt.startup().waitForRecovery();
        QVERIFY2(rt.startup().recoveryNotice().startsWith(QStringLiteral("Your 1 saved run from SynBench was copied here.")),
                 qPrintable(rt.startup().recoveryNotice()));
    }

    // A freshly installed Syntheon with nothing to copy says nothing.
    void noCopyNoNotice()
    {
        QTemporaryDir dir;
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = dir.filePath(QStringLiteral("data"));
        AppRuntime rt(o);
        rt.storageController().open();
        rt.startup().waitForRecovery();
        QVERIFY(!rt.startup().recoveryPending());
    }

    void historyCopyProblemsReachTheFailedDialog()
    {
        QTemporaryDir dir;
        const QString data = dir.filePath(QStringLiteral("data"));
        QDir().mkpath(data);
        QFile garbage(QDir(data).filePath(QStringLiteral("synbench.db")));
        QVERIFY(garbage.open(QIODevice::WriteOnly));
        garbage.write(QByteArray(8192, 'x'));
        garbage.close();
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = data;
        AppRuntime rt(o);
        rt.storageController().open();
        QCOMPARE(rt.storageController().startupKind(), QStringLiteral("failed"));
        const QString message = rt.storageController().startupMessage();
        QVERIFY2(message.contains(QStringLiteral("Your saved runs were not changed.")), qPrintable(message));
        // The file that's at fault, not the new one, which doesn't exist.
        QVERIFY2(message.contains(QDir::toNativeSeparators(garbage.fileName())), qPrintable(message));
        QVERIFY2(!message.contains(QStringLiteral("syntheon.db")), qPrintable(message));
    }

    void runningOldBuildReachesTheFailedDialog()
    {
        QTemporaryDir dir, runtime, picked;
#ifdef Q_OS_UNIX
        const QByteArray realRuntime = qgetenv("XDG_RUNTIME_DIR");
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
        const auto restoreRuntime = qScopeGuard([&] { qputenv("XDG_RUNTIME_DIR", realRuntime); });
        const QString lockDir = runtime.filePath(QStringLiteral("synbench"));
#else
        QVERIFY(legacy::defaultDataDir().contains(QLatin1StringView("qttest"), Qt::CaseInsensitive));
        const QString lockDir = QDir(legacy::defaultDataDir()).filePath(QStringLiteral("run"));
        const auto removeFolder = qScopeGuard([] { QDir(legacy::defaultDataDir()).removeRecursively(); });
#endif
        QVERIFY(QDir().mkpath(lockDir));
        const QString data = dir.filePath(QStringLiteral("data"));
        QDir().mkpath(data);
        const QString oldDb = QDir(data).filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(oldDb).ok());
        const QString pickedDb = QDir(picked.path()).filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(pickedDb).ok());

        QLockFile oldBuild(legacy::lockFilePath(data));
        QVERIFY(oldBuild.tryLock(0));
        QLockFile oldBuildPicked(legacy::lockFilePath(picked.path()));
        QVERIFY(oldBuildPicked.tryLock(0));

        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = data;
        AppRuntime rt(o);
        rt.storageController().open();
        const QString message = QStringLiteral("SynBench is still running with your saved runs. Close it, then start Syntheon again.");
        QCOMPARE(rt.storageController().startupKind(), QStringLiteral("failed"));
        QCOMPARE(rt.storageController().startupMessage(), message);
        QCOMPARE(rt.storageController().openHistoryIn(picked.path()), message);  // apply()
    }

    // The real per-user settings path, in the test-mode location.
    void oldSettingsArriveBeforeAnythingReadsThem()
    {
        const QString legacyIni = legacy::settingsFile(), newIni = SettingsStore().fileName();
        QVERIFY(legacyIni.contains(QLatin1StringView("qttest")) && newIni.contains(QLatin1StringView("qttest")));
        QFile::remove(newIni);
        {
            SettingsStore old(legacyIni);
            old.setTheme(QStringLiteral("dark"));
            QVERIFY(old.sync());
        }
        const auto cleanup = qScopeGuard([&] { QFile::remove(legacyIni); QFile::remove(newIni); });
        AppRuntime rt(AppRuntime::Options{});
        QCOMPARE(rt.settingsStore().fileName(), newIni);
        QCOMPARE(rt.settings().theme(), QStringLiteral("dark"));
        QVERIFY(QFile::exists(newIni));
    }

    // A failed copy means defaults for the session: the old file is never the live
    // store, and no path in it is followed. The next launch copies it after all.
    void settingsCopyFailureRunsOnDefaults_data()
    {
        QTest::addColumn<bool>("customFolder");
        QTest::newRow("old default folder") << false;
        QTest::newRow("custom folder") << true;
    }
    void settingsCopyFailureRunsOnDefaults()
    {
        QFETCH(bool, customFolder);
        const QString legacyIni = legacy::settingsFile(), newIni = SettingsStore().fileName();
        QVERIFY(legacyIni.contains(QLatin1StringView("qttest")) && newIni.contains(QLatin1StringView("qttest")));
        QVERIFY(legacy::defaultDataDir().contains(QLatin1StringView("qttest"), Qt::CaseInsensitive));
        QVERIFY(Storage::defaultDataDir().contains(QLatin1StringView("qttest"), Qt::CaseInsensitive));
        const QString newFolder = QFileInfo(newIni).path();
        QVERIFY(QDir(newFolder).removeRecursively());
        QVERIFY(QDir(Storage::defaultDataDir()).removeRecursively());

        QTemporaryDir custom;
        const QString oldFolder = customFolder ? custom.path() : legacy::defaultDataDir();
        QVERIFY(QDir().mkpath(oldFolder));
        {
            SettingsStore old(legacyIni);
            old.setDataLocation(oldFolder);
            old.setTheme(QStringLiteral("dark"));
            QVERIFY(old.sync());
        }
        const auto cleanup = qScopeGuard([&] {
            QFile::remove(legacyIni);
            QFile::remove(newFolder);
            QDir(newFolder).removeRecursively();
            QDir(legacy::defaultDataDir()).removeRecursively();
            QDir(Storage::defaultDataDir()).removeRecursively();
        });
        QFile iniFile(legacyIni);
        QVERIFY(iniFile.open(QIODevice::ReadOnly));
        const QByteArray before = iniFile.readAll();
        iniFile.close();

        QFile blocker(newFolder);  // a file where the new folder has to go
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Couldn't copy the settings")));
        {
            AppRuntime rt(AppRuntime::Options{});
            QCOMPARE(rt.settingsStore().fileName(), newIni);
            QCOMPARE(rt.settings().theme(), QStringLiteral("system"));
            QVERIFY(rt.settingsStore().dataLocation().isEmpty());
            rt.storageController().open();
            rt.startup().waitForRecovery();
            QCOMPARE(rt.storageController().startupKind(), QStringLiteral("none"));
            QCOMPARE(rt.storage().dataDir(), Storage::defaultDataDir());
        }
        QVERIFY(!QFile::exists(QDir(oldFolder).filePath(QStringLiteral("syntheon.db"))));
        QVERIFY(iniFile.open(QIODevice::ReadOnly));
        QCOMPARE(iniFile.readAll(), before);
        iniFile.close();

        QVERIFY(QFile::remove(newFolder));
        AppRuntime rt(AppRuntime::Options{});
        QCOMPARE(rt.settings().theme(), QStringLiteral("dark"));
        QCOMPARE(rt.settingsStore().dataLocation(), customFolder ? custom.path() : QString());
    }

    // A set settings file means tests, the tour or the dev hooks: the old location is none of their business.
    void explicitSettingsFileNeverReadsTheOldLocation()
    {
        const QString legacyIni = legacy::settingsFile(), defaultIni = SettingsStore().fileName();
        QVERIFY(legacyIni.contains(QLatin1StringView("qttest")) && defaultIni.contains(QLatin1StringView("qttest")));
        QFile::remove(defaultIni);
        {
            SettingsStore old(legacyIni);
            old.setTheme(QStringLiteral("dark"));
            QVERIFY(old.sync());
        }
        const auto cleanup = qScopeGuard([&] { QFile::remove(legacyIni); });
        QTemporaryDir dir;
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        AppRuntime rt(o);
        QCOMPARE(rt.settings().theme(), QStringLiteral("system"));
        QVERIFY(!QFile::exists(defaultIni));
    }

    // AP-5 #4: a run earlier in the session must not switch recovery off.
    void recoveryStillRunsAfterARunThisSession()
    {
        tests::setStubTimeScale(0.02);
        QTemporaryDir dir;
        AppRuntime::Options o;
        o.noElevation = true;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = dir.filePath(QStringLiteral("a"));
        AppRuntime rt(o);
        QVERIFY(rt.claimInstance());
        rt.start();
        QTRY_VERIFY_WITH_TIMEOUT(!rt.run().probing(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!rt.startup().recovering(), 10000);
        rt.run().toggle(QStringLiteral("ai"));
        QSignalSpy finished(&rt.run(), &RunController::runFinished);
        QVERIFY(rt.run().start());
        QVERIFY(finished.wait(60000));
        QVERIFY(rt.run().hasRun());

        const QString other = dir.filePath(QStringLiteral("b"));
        QDir().mkpath(other);
        {
            const QString path = QDir(other).filePath(QStringLiteral("syntheon.db"));
            QVERIFY(Database::migrate(path).ok());
            Database::ScopedConnection c(path, QStringLiteral("seed"));
            RunRecord r;
            r.uuid = QUuid::createUuid();
            r.status = RunStatus::Running;
            r.startedAt = QDateTime::currentDateTimeUtc().addSecs(-3600);
            TestResult t = TestResult::withStatus(Status::NotRun, QStringLiteral("Queued"));
            t.testId = QStringLiteral("ram");
            r.tests = {t};
            QVERIFY(RunRepository(c.db()).saveRun(r));
        }
        QCOMPARE(rt.storageController().openHistoryIn(other), QString());
        QTRY_VERIFY_WITH_TIMEOUT(rt.startup().recoveryPending(), 10000);
        tests::setStubTimeScale(0);
    }

    // The two holders of the history lock don't release each other's.
    void storageLocksAreKeyed()
    {
        QTemporaryDir dir;
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = dir.filePath(QStringLiteral("a"));
        AppRuntime rt(o);
        StorageController &c = rt.storageController();
        c.setLocked(true, QStringLiteral("run"));
        {
            StorageController::ScopedLock scoped(c, QStringLiteral("recovery"), QStringLiteral("checking"));
            QVERIFY(c.lockedBy(QStringLiteral("recovery")));
            c.setLocked(false);  // the run ends first
            QVERIFY(c.locked());  // recovery still holds it
        }
        QVERIFY(!c.locked());
        QVERIFY(!rt.storage().isBusy());
    }

    // AP-5 re-check: the same leftover failing on every pass is reported once.
    void leftoverWarningIsNotRepeated()
    {
        QTemporaryDir dir;
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = dir.filePath(QStringLiteral("a"));
        AppRuntime rt(o);
        StartupController &st = rt.startup();
        RecoveryReport report;
        report.errors = {QStringLiteral("couldn't delete /tmp/syntheon-x/scratch")};
        st.finishRecovery(report);
        st.dismissRecoveryNotice();
        st.finishRecovery(report);  // the same problem again, next pass
        QVERIFY(!st.recoveryPending());
        report.errors = {QStringLiteral("a different problem")};
        st.finishRecovery(report);
        QVERIFY(st.recoveryPending());
        QCOMPARE(st.recoveryNotice().count(QStringLiteral("leftovers")), 1);
    }

    void firstRunDialogData()
    {
        QTemporaryDir dir;
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = dir.filePath(QStringLiteral("a"));
        AppRuntime rt(o);
        StartupController &st = rt.startup();
        QVERIFY(st.firstRunNeeded());
        // One row per test; before probing they say "Checking…", and no
        // model-pack pointer exists yet (Phase 6).
        const QVariantList rows = st.hardwareCheck();
        QCOMPARE(rows.size(), 7);
        for (const QVariant &r : rows)
            QVERIFY(r.toMap().value(QStringLiteral("hint")).toString().isEmpty());
        QSignalSpy spy(&st, &StartupController::firstRunChanged);
        st.finishFirstRun();
        QVERIFY(!st.firstRunNeeded());
        QCOMPARE(spy.count(), 1);
    }

    // Zeb #3: a damaged history file says where it is and why, and stays untouched.
    void damagedHistorySaysWhereAndWhyAndIsLeftAlone()
    {
        QTemporaryDir dir;
        const QString data = dir.filePath(QStringLiteral("data"));
        QDir().mkpath(data);
        const QString db = QDir(data).filePath(QStringLiteral("syntheon.db"));
        const QByteArray garbage = QByteArray("this is not an SQLite database, just text ").repeated(200);
        {
            QFile f(db);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(garbage);
        }
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = data;
        AppRuntime rt(o);
        StorageController &c = rt.storageController();
        c.open();
        QCOMPARE(c.startupKind(), QStringLiteral("failed"));
        const QString msg = c.startupMessage();
        QVERIFY2(msg.contains(QDir::toNativeSeparators(db)), qPrintable(msg));
        QVERIFY2(msg.contains(QStringLiteral("damaged or isn't a Syntheon history")), qPrintable(msg));
        QVERIFY2(msg.contains(QStringLiteral("(file is not a database)")), qPrintable(msg));  // SQLite's own reason, alone
        QVERIFY2(!msg.contains(QStringLiteral("Unable to fetch row")), qPrintable(msg));
        QVERIFY2(msg.contains(QStringLiteral("left untouched")), qPrintable(msg));
        QFile f(db);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), garbage);  // byte for byte
        QVERIFY(!QFile::exists(db + QStringLiteral("-wal")));
        QVERIFY(QDir(data).entryList({QStringLiteral("syntheon.db.bak*")}).isEmpty());
    }

    void storageControllerBasics()
    {
        QTemporaryDir dir;
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = dir.filePath(QStringLiteral("a"));
        AppRuntime rt(o);
        StorageController &c = rt.storageController();
        c.open();
        rt.startup().waitForRecovery();
        QCOMPARE(c.startupKind(), QStringLiteral("none"));
        QCOMPARE(c.runCount(), 0);
        QVERIFY(c.dbPath().endsWith(QStringLiteral("syntheon.db")));
        QVERIFY(c.sizeText().contains(QLatin1StringView("B")));

        RunRepository repo(rt.storage().connection());
        QVERIFY(repo.saveRun(run()));
        QVERIFY(repo.saveRun(run()));
        QCOMPARE(c.runCount(), 2);

        // A run pins the location and Clear history.
        c.setLocked(true, QStringLiteral("A run is in progress."));
        QVERIFY(c.locked());
        QCOMPARE(c.lockedReason(), QStringLiteral("A run is in progress."));
        QVERIFY(!c.clearHistory().isEmpty());
        QVERIFY(!c.moveHistoryTo(dir.filePath(QStringLiteral("elsewhere"))).isEmpty());
        QCOMPARE(c.runCount(), 2);
        c.setLocked(false);

        QSignalSpy changed(&c, &StorageController::historyChanged);
        QCOMPARE(c.clearHistory(), QString());
        QCOMPARE(c.runCount(), 0);
        QVERIFY(changed.count() >= 1);
        QCOMPARE(c.urlToPath(QUrl::fromLocalFile(QStringLiteral("/tmp/x"))), QStringLiteral("/tmp/x"));
    }

    void moveAndOpenExisting()
    {
        QTemporaryDir dir;
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = dir.filePath(QStringLiteral("a"));
        AppRuntime rt(o);
        StorageController &c = rt.storageController();
        c.open();
        rt.startup().waitForRecovery();
        RunRepository(rt.storage().connection()).saveRun(run());

        const QString b = dir.filePath(QStringLiteral("b"));
        QVERIFY(QDir().mkpath(b));
        QCOMPARE(c.inspectFolder(b), QStringLiteral("empty"));
        QCOMPARE(c.inspectFolder(dir.filePath(QStringLiteral("missing"))), QStringLiteral("unusable"));
        QCOMPARE(c.moveHistoryTo(b), QString());  // moved, never merged
        // A new location starts the recovery check, which holds the storage
        // lock in the background: let it finish (under load it can still be
        // running when the next location change comes, which is refused).
        rt.startup().waitForRecovery();
        QCOMPARE(c.runCount(), 1);
        QVERIFY(c.dataLocation().endsWith(QStringLiteral("b")));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("a/syntheon.db"))));

        // Another folder that already has a history: offered, not merged.
        const QString other = dir.filePath(QStringLiteral("other"));
        {
            AppRuntime::Options po;
            po.settingsFile = dir.filePath(QStringLiteral("other.ini"));
            po.dataFolder = other;
            AppRuntime second(po);
            second.storageController().open();
            RunRepository r(second.storage().connection());
            r.saveRun(run());
            r.saveRun(run());
            r.saveRun(run());
        }
        QCOMPARE(c.inspectFolder(other), QStringLiteral("database"));
        QVERIFY(!c.moveHistoryTo(other).isEmpty());  // refused: no merging
        QCOMPARE(c.runCount(), 1);
        // While something holds the storage lock the location can't change,
        // and the message says who holds it (AP-5 L6).
        c.lock(QStringLiteral("recovery"), QStringLiteral("Checking earlier runs…"));
        QVERIFY(c.openHistoryIn(other).contains(QLatin1StringView("earlier runs are being checked")));
        QVERIFY(c.moveHistoryTo(other).contains(QLatin1StringView("earlier runs are being checked")));
        c.unlock(QStringLiteral("recovery"));
        c.lock(QStringLiteral("run"), QStringLiteral("A run is active"));
        QVERIFY(c.openHistoryIn(other).contains(QLatin1StringView("while a run is active")));
        c.unlock(QStringLiteral("run"));
        QCOMPARE(c.openHistoryIn(other), QString());
        QCOMPARE(c.runCount(), 3);  // that folder's own history
        QVERIFY(QFile::exists(QDir(b).filePath(QStringLiteral("syntheon.db"))));  // the old one untouched
    }

    void launchProblems()
    {
        QTemporaryDir dir;
        // A location that vanished (a removed drive).
        {
            AppRuntime::Options o;
            o.settingsFile = dir.filePath(QStringLiteral("s1.ini"));
            AppRuntime rt(o);
            rt.settingsStore().setDataLocation(dir.filePath(QStringLiteral("removed-usb")));
            StorageController &c = rt.storageController();
            QSignalSpy problem(&c, &StorageController::startupProblemChanged);
            c.open();
            QCOMPARE(c.startupKind(), QStringLiteral("location"));
            QVERIFY(c.startupMessage().contains(QLatin1StringView("isn't available")));
            QVERIFY(problem.count() >= 1);
            QCOMPARE(c.useDefaultLocation(), QString());  // Use default location
            QCOMPARE(c.startupKind(), QStringLiteral("none"));
            // Qt's test location: tests/support/test_isolation.cpp enables test
            // mode for every test binary and aborts unless it took.
            QDir(Storage::defaultDataDir()).removeRecursively();
        }
        // A history from a newer Syntheon: refused, untouched, Choose another.
        {
            const QString newer = dir.filePath(QStringLiteral("newer"));
            QDir().mkpath(newer);
            {
                Database::ScopedConnection conn(QDir(newer).filePath(QStringLiteral("syntheon.db")), QStringLiteral("t"));
                QSqlQuery(conn.db()).exec(QStringLiteral("PRAGMA user_version = 9"));
            }
            AppRuntime::Options o;
            o.settingsFile = dir.filePath(QStringLiteral("s2.ini"));
            o.dataFolder = newer;
            AppRuntime rt(o);
            StorageController &c = rt.storageController();
            c.open();
            QCOMPARE(c.startupKind(), QStringLiteral("newer"));
            QVERIFY(c.startupMessage().contains(QLatin1StringView("newer Syntheon")));
            const QString fresh = dir.filePath(QStringLiteral("fresh"));
            QDir().mkpath(fresh);
            QCOMPARE(c.openHistoryIn(fresh), QString());  // Choose…
            QCOMPARE(c.startupKind(), QStringLiteral("none"));
            QVERIFY(!c.openHistoryIn(newer).isEmpty());   // still refused
        }
    }

    // The smoke verdict (batch D CI): Ok passes; RAM Partial only for
    // loaded latency N/A "needs at least two cores" passes (a one-core
    // runner); every other non-Ok result fails.
    void smokeVerdict()
    {
        const auto ram = [](Status loaded, const QString &reason, Status bandwidth = Status::Ok) {
            TestResult r;
            r.testId = QStringLiteral("ram");
            r.status = Status::Partial;
            r.subResults = {{QStringLiteral("latency"), Status::Ok, {}, 1000.0},
                            {QStringLiteral("bandwidth"), bandwidth, {}, 1000.0},
                            {QStringLiteral("loaded"), loaded, reason, std::nullopt}};
            return r;
        };
        const QString twoCores = QStringLiteral("needs at least two cores");
        TestResult ok;
        ok.testId = QStringLiteral("ram");
        QVERIFY(smokeAccepts(ok));
        QVERIFY(smokeAccepts(ram(Status::NotAvailable, twoCores)));
        // Anything else fails.
        QVERIFY(!smokeAccepts(ram(Status::NotAvailable, twoCores, Status::Error)));
        QVERIFY(!smokeAccepts(ram(Status::Error, twoCores)));
        QVERIFY(!smokeAccepts(ram(Status::NotAvailable, QStringLiteral("needs the latency buffer (no memory)"))));
        QVERIFY(!smokeAccepts(ram(Status::Ok, {}, Status::Error)));
        TestResult noneFailed = ram(Status::Ok, {});
        QVERIFY(!smokeAccepts(noneFailed));  // Partial with nothing N/A: not this case
        // The GPU test: NotAvailable (no API could run on this runner) is accepted; an API Error or any other Partial is not.
        TestResult gpuNa;
        gpuNa.testId = QStringLiteral("gpu");
        gpuNa.status = Status::NotAvailable;
        gpuNa.statusReason = QStringLiteral("No Vulkan driver is installed; DirectX is Windows only");
        QVERIFY(smokeAccepts(gpuNa));
        TestResult gpuError = gpuNa;
        gpuError.status = Status::Error;
        QVERIFY(!smokeAccepts(gpuError));
        TestResult gpuPartial = gpuNa;  // an API Error: Partial with an Error 3D sub-result
        gpuPartial.status = Status::Partial;
        gpuPartial.subResults = {{QStringLiteral("scenes3d"), Status::Error, QStringLiteral("DirectX 11 failed in Geometry: x"), std::nullopt}};
        QVERIFY(!smokeAccepts(gpuPartial));
        // Task 32: every part exists now, so any GPU Partial is rejected: a missing fill-rate or native 2D part, or an error in one.
        TestResult gpuFill;
        gpuFill.testId = QStringLiteral("gpu");
        gpuFill.status = Status::Partial;
        gpuFill.subResults = {{QStringLiteral("scenes3d"), Status::Ok, {}, 1000.0},
                              {QStringLiteral("native2d"), Status::Ok, {}, 1000.0},
                              {QStringLiteral("fill2d"), Status::NotAvailable, QStringLiteral("not built yet (Phase 4a-4)"), std::nullopt}};
        QVERIFY(!smokeAccepts(gpuFill));
        TestResult gpuFillError = gpuFill;
        gpuFillError.subResults[2] = {QStringLiteral("fill2d"), Status::Error, QStringLiteral("OpenGL failed in Sprites: x"), std::nullopt};
        QVERIFY(!smokeAccepts(gpuFillError));
        TestResult gpuNativeError = gpuFill;
        gpuNativeError.subResults[1] = {QStringLiteral("native2d"), Status::Error, QStringLiteral("Qt raster failed in Text: x"), std::nullopt};
        QVERIFY(!smokeAccepts(gpuNativeError));
        TestResult gpuOk = gpuFill;  // all three parts Ok: an Ok result
        gpuOk.status = Status::Ok;
        QVERIFY(smokeAccepts(gpuOk));
        TestResult notGpu = gpuNa;
        notGpu.testId = QStringLiteral("cpu");
        QVERIFY(!smokeAccepts(notGpu));
        TestResult cpu = ram(Status::NotAvailable, twoCores);
        cpu.testId = QStringLiteral("cpu");
        QVERIFY(!smokeAccepts(cpu));
        TestResult err;
        err.testId = QStringLiteral("ram");
        err.status = Status::Error;
        QVERIFY(!smokeAccepts(err));
    }

    void smokeRuntimeUsesATemporaryHistory()
    {
        AppRuntime::Options o;
        o.smoke = true;
        AppRuntime rt(o);
        rt.storageController().open();
        QCOMPARE(rt.storageController().startupKind(), QStringLiteral("none"));
        // Not the user's history.
        QVERIFY(!QDir::fromNativeSeparators(rt.storageController().dbPath()).startsWith(Storage::defaultDataDir()));
        QVERIFY(rt.storageController().dbPath().contains(QLatin1StringView("data")));
        QVERIFY(!rt.app().privileged().isAvailable());  // no elevation in smoke runs
    }
};

QTEST_MAIN(TestControllers)
#include "tst_controllers.moc"
