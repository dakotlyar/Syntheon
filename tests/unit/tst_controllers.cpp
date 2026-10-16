// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/controllers/settings_controller.h"
#include "app/controllers/storage_controller.h"
#include "storage/database.h"
#include "storage/run_repository.h"

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
