// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/single_instance.h"
#include "storage/database.h"
#include "storage/run_repository.h"
#include "storage/settings_store.h"
#include "storage/storage.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>

#include <future>

using namespace sb;

namespace {

RunRecord staleRunning()
{
    RunRecord r;
    r.uuid = QUuid::createUuid();
    r.status = RunStatus::Running;
    r.startedAt = QDateTime::currentDateTimeUtc().addSecs(-60);
    TestResult t = TestResult::withStatus(Status::NotRun, QStringLiteral("Queued"));
    t.testId = QStringLiteral("cpu");
    r.tests = {t};
    return r;
}

} // namespace

// One SynBench per history folder: the lock, the focus request, the stale lock
// after a crash, two folders side by side, and recovery leaving a live run
// alone.
class TestSingleInstance : public QObject
{
    Q_OBJECT

private slots:
    void secondStorageOnTheSameFolderIsRefused()
    {
        QTemporaryDir dir;
        SettingsStore s1(dir.filePath(QStringLiteral("1.ini"))), s2(dir.filePath(QStringLiteral("2.ini")));
        const QString folder = dir.filePath(QStringLiteral("data"));
        QDir().mkpath(folder);
        s1.setDataLocation(folder);
        s2.setDataLocation(folder);
        {
            Storage a(s1);
            QVERIFY(a.open().ok());
            Storage b(s2);
            QCOMPARE(b.open().status, Storage::OpenStatus::InUse);
            QVERIFY(!b.isOpen());
        }
        // Released when the first one goes away.
        Storage c(s2);
        QVERIFY(c.open().ok());
    }

    void differentFoldersCoexist()
    {
        QTemporaryDir dir;
        SettingsStore s1(dir.filePath(QStringLiteral("1.ini"))), s2(dir.filePath(QStringLiteral("2.ini")));
        s1.setDataLocation(dir.filePath(QStringLiteral("a")));
        s2.setDataLocation(dir.filePath(QStringLiteral("b")));
        QDir().mkpath(dir.filePath(QStringLiteral("a")));
        QDir().mkpath(dir.filePath(QStringLiteral("b")));
        Storage a(s1), b(s2);
        QVERIFY(a.open().ok());
        QVERIFY(b.open().ok());
        QVERIFY(SingleInstance::nameFor(a.dataDir()) != SingleInstance::nameFor(b.dataDir()));
    }

    void switchingToAFolderInUseIsRefusedAndKeepsTheOldHistory()
    {
        QTemporaryDir dir;
        SettingsStore s1(dir.filePath(QStringLiteral("1.ini"))), s2(dir.filePath(QStringLiteral("2.ini")));
        const QString a = dir.filePath(QStringLiteral("a")), b = dir.filePath(QStringLiteral("b"));
        QDir().mkpath(a);
        QDir().mkpath(b);
        s1.setDataLocation(a);
        s2.setDataLocation(b);
        Storage first(s1), second(s2);
        QVERIFY(first.open().ok());
        QVERIFY(second.open().ok());
        QCOMPARE(first.switchTo(b).status, Storage::OpenStatus::InUse);
        QVERIFY(first.isOpen());
        QCOMPARE(first.dataDir(), a);
        // And after the other lets go, the lock on `a` is still ours and b is free.
        Storage third(s2);
        QCOMPARE(third.open().status, Storage::OpenStatus::InUse);
    }

    // A lock left by a crashed process (its pid no longer exists) is taken over.
    void staleLockAfterACrashIsTakenOver()
    {
        QTemporaryDir dir;
        SettingsStore s(dir.filePath(QStringLiteral("s.ini")));
        const QString folder = dir.filePath(QStringLiteral("data"));
        QDir().mkpath(folder);
        s.setDataLocation(folder);
        QFile lock(QDir(folder).filePath(QStringLiteral("synbench.lock")));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        // QLockFile's format: pid, application name, host name.
        lock.write(QByteArray("2147483000\nSynBench\n") + QSysInfo::machineHostName().toUtf8() + "\n");
        lock.close();
        Storage st(s);
        QVERIFY2(st.open().ok(), "a lock owned by a dead process must not block the app");
    }

    void aSecondLaunchAsksTheFirstToComeToTheFront()
    {
        QTemporaryDir dir;
        const QString folder = dir.filePath(QStringLiteral("data"));
        SingleInstance first;
        QVERIFY(first.listen(folder));
        QSignalSpy focus(&first, &SingleInstance::focusRequested);
        // The request is blocking socket I/O, so it comes from another thread
        // while this one runs the server's event loop.
        auto answered = std::async(std::launch::async, [&] { return SingleInstance::requestFocus(folder, 5000); });
        QTRY_COMPARE_WITH_TIMEOUT(focus.count(), 1, 5000);
        QVERIFY(answered.get());
        // A different folder's copy isn't disturbed.
        QVERIFY(!SingleInstance::requestFocus(dir.filePath(QStringLiteral("other")), 300));
        QCOMPARE(focus.count(), 1);
    }

    // Recovery must never touch the run of a copy that is alive: the second
    // copy doesn't even open the database.
    void recoveryNeverTouchesALiveCopysRun()
    {
        QTemporaryDir dir;
        const QString folder = dir.filePath(QStringLiteral("data"));
        AppRuntime::Options o;
        o.noElevation = true;
        o.settingsFile = dir.filePath(QStringLiteral("a.ini"));
        o.dataFolder = folder;
        AppRuntime live(o);
        QVERIFY(live.claimInstance());
        live.startup().waitForRecovery();
        const RunRecord run = staleRunning();  // stands for the live copy's own run
        QVERIFY(RunRepository(live.storage().connection()).saveRun(run));

        AppRuntime::Options o2 = o;
        o2.settingsFile = dir.filePath(QStringLiteral("b.ini"));
        // The second copy asks the first, blocking: do it off this thread.
        auto second = std::async(std::launch::async, [&] {
            AppRuntime rt(o2);
            const bool claimed = rt.claimInstance();
            rt.startup().recover();
            return std::make_pair(claimed, rt.redirectedToRunningInstance());
        });
        QSignalSpy focus(&live.instance(), &SingleInstance::focusRequested);
        QTRY_COMPARE_WITH_TIMEOUT(focus.count(), 1, 10000);
        const auto result = second.get();
        QVERIFY(!result.first);
        QVERIFY(result.second);
        QCOMPARE(RunRepository(live.storage().connection()).load(run.uuid)->status, RunStatus::Running);
    }

    // The real executable: a second launch on the same folder exits 0 and
    // the first gets the request; a different folder starts normally.
    void secondProcessExitsCleanlyAndTheFirstIsToldToRaise()
    {
#ifdef SYNBENCH_EXE
        QTemporaryDir dir;
        auto launch = [&](QProcess &p, const QString &folder, const QString &ini) {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
            env.insert(QStringLiteral("SYNBENCH_NO_ELEVATION"), QStringLiteral("1"));
            env.insert(QStringLiteral("SYNBENCH_DATA_DIR"), folder);
            env.insert(QStringLiteral("SYNBENCH_SETTINGS_FILE"), dir.filePath(ini));
            env.insert(QStringLiteral("QT_LOGGING_RULES"), QStringLiteral("synbench.instance.info=true"));
            p.setProcessEnvironment(env);
            p.setProcessChannelMode(QProcess::MergedChannels);
            p.start(QStringLiteral(SYNBENCH_EXE), {});
        };
        const QString shared = dir.filePath(QStringLiteral("shared"));
        QProcess first;
        launch(first, shared, QStringLiteral("1.ini"));
        QVERIFY(first.waitForStarted());
        QByteArray firstOut;
        QTRY_VERIFY_WITH_TIMEOUT((firstOut += first.readAll()).contains("listening for a second launch"), 30000);

        QProcess second;
        launch(second, shared, QStringLiteral("2.ini"));
        QVERIFY(second.waitForFinished(30000));
        QCOMPARE(second.exitStatus(), QProcess::NormalExit);
        QCOMPARE(second.exitCode(), 0);
        QTRY_VERIFY_WITH_TIMEOUT((firstOut += first.readAll()).contains("another launch asked for this window"), 10000);
        QCOMPARE(first.state(), QProcess::Running);

        // A copy on another folder isn't turned away.
        QProcess other;
        launch(other, dir.filePath(QStringLiteral("elsewhere")), QStringLiteral("3.ini"));
        QByteArray otherOut;
        QTRY_VERIFY_WITH_TIMEOUT((otherOut += other.readAll()).contains("listening for a second launch"), 30000);
        QCOMPARE(other.state(), QProcess::Running);

        for (QProcess *p : {&first, &other}) {
            p->terminate();
            if (!p->waitForFinished(10000))
                p->kill();
        }
#else
        QSKIP("the SynBench executable path isn't known to this test");
#endif
    }
};

QTEST_MAIN(TestSingleInstance)
#include "tst_single_instance.moc"
