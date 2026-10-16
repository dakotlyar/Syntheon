// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/controllers/run_controller.h"
#include "app/controllers/settings_controller.h"
#include "app/controllers/startup_controller.h"
#include "app/controllers/storage_controller.h"
#include "storage/database.h"
#include "storage/run_repository.h"

#include <QCborMap>
#include <QCborValue>
#include <QDir>
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
        st.recover();
        QVERIFY(!st.recoveryPending());  // an empty history: nothing to report

        auto seed = [&](const QString &testId, RunKind kind) {
            RunRecord r;
            r.uuid = QUuid::createUuid();
            r.kind = kind;
            r.status = RunStatus::Running;
            r.startedAt = QDateTime::currentDateTimeUtc().addSecs(-7200);
            r.appVersion = QStringLiteral("2026.11.0");
            TestResult t = TestResult::withStatus(Status::NotRun, QStringLiteral("Queued"));
            t.testId = testId;
            r.tests = {t};
            RunRepository repo(rt.storage().connection());
            const auto id = repo.saveRun(r);
            [&] { QVERIFY(id); }();
            return std::make_pair(r.uuid, *id);
        };

        const auto cut = seed(QStringLiteral("cpu"), RunKind::Standard);
        st.recover();
        QVERIFY(st.recoveryPending());
        QVERIFY(st.recoveryNotice().contains(QStringLiteral("was interrupted")));
        QVERIFY(!rt.storageController().locked());
        {
            RunRepository repo(rt.storage().connection());
            QCOMPARE(repo.load(cut.first)->status, RunStatus::Interrupted);
        }
        st.dismissRecoveryNotice();
        QVERIFY(!st.recoveryPending());
        st.recover();  // nothing left to recover: the notice doesn't come back
        QVERIFY(!st.recoveryPending());

        const auto battery = seed(QStringLiteral("battery"), RunKind::Exclusive);
        {
            RunRepository repo(rt.storage().connection());
            for (int i = 0; i < 3; ++i)
                QVERIFY(repo.addCheckpoint(battery.second, QStringLiteral("battery"), i,
                                           QCborValue(QCborMap{{QStringLiteral("t"), i * 100.0},
                                                               {QStringLiteral("charge"), 100.0 - i * 30}}).toCbor()));
        }
        st.recover();
        QVERIFY(st.recoveryPending());
        QVERIFY2(st.recoveryNotice().contains(QStringLiteral("recovered: 3 h 20 m")), qPrintable(st.recoveryNotice()));
        {
            RunRepository repo(rt.storage().connection());
            QCOMPARE(repo.load(battery.first)->status, RunStatus::Recovered);
        }
        QCOMPARE(StartupController::hoursMinutes((6 * 60 + 42) * 60000LL), QStringLiteral("6 h 42 m"));
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

    void storageControllerBasics()
    {
        QTemporaryDir dir;
        AppRuntime::Options o;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.dataFolder = dir.filePath(QStringLiteral("a"));
        AppRuntime rt(o);
        StorageController &c = rt.storageController();
        c.open();
        QCOMPARE(c.startupKind(), QStringLiteral("none"));
        QCOMPARE(c.runCount(), 0);
        QVERIFY(c.dbPath().endsWith(QStringLiteral("synbench.db")));
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
        RunRepository(rt.storage().connection()).saveRun(run());

        const QString b = dir.filePath(QStringLiteral("b"));
        QVERIFY(QDir().mkpath(b));
        QCOMPARE(c.inspectFolder(b), QStringLiteral("empty"));
        QCOMPARE(c.inspectFolder(dir.filePath(QStringLiteral("missing"))), QStringLiteral("unusable"));
        QCOMPARE(c.moveHistoryTo(b), QString());  // moved, never merged
        QCOMPARE(c.runCount(), 1);
        QVERIFY(c.dataLocation().endsWith(QStringLiteral("b")));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("a/synbench.db"))));

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
        QCOMPARE(c.openHistoryIn(other), QString());
        QCOMPARE(c.runCount(), 3);  // that folder's own history
        QVERIFY(QFile::exists(QDir(b).filePath(QStringLiteral("synbench.db"))));  // the old one untouched
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
            QDir(Storage::defaultDataDir()).removeRecursively();
        }
        // A history from a newer SynBench: refused, untouched, Choose another.
        {
            const QString newer = dir.filePath(QStringLiteral("newer"));
            QDir().mkpath(newer);
            {
                Database::ScopedConnection conn(QDir(newer).filePath(QStringLiteral("synbench.db")), QStringLiteral("t"));
                QSqlQuery(conn.db()).exec(QStringLiteral("PRAGMA user_version = 9"));
            }
            AppRuntime::Options o;
            o.settingsFile = dir.filePath(QStringLiteral("s2.ini"));
            o.dataFolder = newer;
            AppRuntime rt(o);
            StorageController &c = rt.storageController();
            c.open();
            QCOMPARE(c.startupKind(), QStringLiteral("newer"));
            QVERIFY(c.startupMessage().contains(QLatin1StringView("newer SynBench")));
            const QString fresh = dir.filePath(QStringLiteral("fresh"));
            QDir().mkpath(fresh);
            QCOMPARE(c.openHistoryIn(fresh), QString());  // Choose…
            QCOMPARE(c.startupKind(), QStringLiteral("none"));
            QVERIFY(!c.openHistoryIn(newer).isEmpty());   // still refused
        }
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
