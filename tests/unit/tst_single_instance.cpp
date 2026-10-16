// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/single_instance.h"
#include "storage/database.h"
#include "storage/instance_paths.h"
#include "storage/run_repository.h"
#include "storage/settings_store.h"
#include "storage/storage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>

#include <future>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

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

private:
    QTemporaryDir m_runtime;  // a private $XDG_RUNTIME_DIR for the whole test

private slots:
    void initTestCase()
    {
        QVERIFY(m_runtime.isValid());
        qputenv("XDG_RUNTIME_DIR", m_runtime.path().toUtf8());
    }

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
        QFile lock(instance::lockFilePath(folder));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        // QLockFile's format: pid, application name, host name.
        lock.write(QByteArray("2147483000\nSynBench\n") + QSysInfo::machineHostName().toUtf8() + "\n");
        lock.close();
        Storage st(s);
        QVERIFY2(st.open().ok(), "a lock owned by a dead process must not block the app");
    }

    // The lock isn't in the history folder (it may be synced) but in a private
    // per-user directory; the socket is there too.
    void lockAndSocketLiveInAPrivateStateDir()
    {
        QTemporaryDir dir;
        SettingsStore s(dir.filePath(QStringLiteral("s.ini")));
        const QString folder = dir.filePath(QStringLiteral("data"));
        QDir().mkpath(folder);
        s.setDataLocation(folder);
        Storage st(s);
        QVERIFY(st.open().ok());
        QVERIFY(!QDir(folder).entryList({QStringLiteral("*.lock")}).contains(QStringLiteral("synbench.lock")));
        QVERIFY(QFile::exists(instance::lockFilePath(folder)));
        SingleInstance si;
        QVERIFY(si.listen(folder));
#ifdef Q_OS_UNIX
        // Linux: $XDG_RUNTIME_DIR/synbench (0700, ours), the socket is a file in it.
        QVERIFY(instance::lockFilePath(folder).startsWith(m_runtime.path()));
        const QFileInfo state(instance::stateDir());
        QCOMPARE(state.ownerId(), uint(geteuid()));
        QVERIFY(!(state.permissions() & (QFile::ReadGroup | QFile::ReadOther | QFile::WriteGroup | QFile::WriteOther)));
        QVERIFY(SingleInstance::nameFor(folder).startsWith(m_runtime.path()));
        QVERIFY(QFile::exists(SingleInstance::nameFor(folder)));
#else
        // Windows: the lock file is under the per-user local data folder
        // (%LOCALAPPDATA%\SynBench\run), NOT in the history folder, named by the
        // folder's hash; the socket is a named pipe (a name, not a file).
        const QString lock = QDir::fromNativeSeparators(instance::lockFilePath(folder));
        const QString local = QDir::fromNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
        const QByteArray why = ("lock path was '" + lock + "', local data folder '" + local + "'").toLocal8Bit();
        QVERIFY2(lock.startsWith(local + QStringLiteral("/SynBench/run/"), Qt::CaseInsensitive), why.constData());
        QVERIFY2(!lock.startsWith(QDir::fromNativeSeparators(folder), Qt::CaseInsensitive), why.constData());
        QVERIFY2(lock.contains(instance::key(folder)), why.constData());
        QVERIFY(QFile::exists(instance::lockFilePath(folder)));
        const QString pipe = SingleInstance::nameFor(folder);
        QVERIFY2(pipe.startsWith(QStringLiteral("synbench-")) && !pipe.contains(QLatin1Char('\\')) && !pipe.contains(QLatin1Char('/')),
                 qPrintable(pipe));
#endif
    }

    // A symlinked path is the same history: same key, same lock.
    void symlinkedPathIsTheSameInstance()
    {
#ifdef Q_OS_WIN
        QSKIP("QFile::link makes a .lnk shortcut on Windows, not a symbolic link");
#endif
        QTemporaryDir dir;
        const QString real = dir.filePath(QStringLiteral("real"));
        const QString link = dir.filePath(QStringLiteral("link"));
        QDir().mkpath(real);
        QVERIFY(QFile::link(real, link));
        QCOMPARE(instance::key(real), instance::key(link));
        QCOMPARE(SingleInstance::nameFor(real), SingleInstance::nameFor(link));
        SettingsStore s1(dir.filePath(QStringLiteral("1.ini"))), s2(dir.filePath(QStringLiteral("2.ini")));
        s1.setDataLocation(real);
        s2.setDataLocation(link);
        Storage a(s1), b(s2);
        QVERIFY(a.open().ok());
        QCOMPARE(b.open().status, Storage::OpenStatus::InUse);
    }

    // A lock whose owner is on another host (a hostname change, or a copied
    // state dir) can't be proven stale: it blocks, and Take over clears it.
    void foreignHostLockBlocksUntilTakenOver()
    {
        QTemporaryDir dir;
        SettingsStore s(dir.filePath(QStringLiteral("s.ini")));
        const QString folder = dir.filePath(QStringLiteral("data"));
        QDir().mkpath(folder);
        s.setDataLocation(folder);
        QFile lock(instance::lockFilePath(folder));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.write(QByteArray::number(QCoreApplication::applicationPid()) + "\nSynBench\nsome-other-host\n");  // a live pid, another host
        lock.close();
        Storage st(s);
        QCOMPARE(st.open().status, Storage::OpenStatus::InUse);
        QVERIFY(st.takeOverLock().ok());
        QVERIFY(st.isOpen());
    }

    // A held lock with nobody listening (a hung copy, or a lock left behind):
    // the runtime doesn't exit silently and doesn't claim success, it reports
    // the problem for the dialog, and Take over continues.
    void heldLockWithoutAnAnswerOffersTakeOver()
    {
#ifdef Q_OS_WIN
        // Windows won't delete a lock file its own holder still has open; a real
        // stale lock (the holder crashed) isn't held, which the stale-lock and
        // foreign-host tests cover.
        QSKIP("the holder here is in the same process and keeps the lock file open");
#endif
        QTemporaryDir dir;
        const QString folder = dir.filePath(QStringLiteral("data"));
        QDir().mkpath(folder);
        SettingsStore s1(dir.filePath(QStringLiteral("holder.ini")));
        s1.setDataLocation(folder);
        Storage holder(s1);  // holds the lock, runs no socket
        QVERIFY(holder.open().ok());

        AppRuntime::Options o;
        o.noElevation = true;
        o.focusWaitMs = 200;
        o.settingsFile = dir.filePath(QStringLiteral("b.ini"));
        o.dataFolder = folder;
        AppRuntime rt(o);
        QVERIFY(rt.claimInstance());                     // not "redirected": nobody answered
        QVERIFY(!rt.redirectedToRunningInstance());
        QCOMPARE(rt.storageController().startupKind(), QStringLiteral("inuse"));
        QVERIFY(!rt.storage().isOpen());

        // Quit's message and exit code are the QML dialog's job (tested there);
        // Take over opens the history and moves on.
        QCOMPARE(rt.storageController().takeOver(), QString());
        QCOMPARE(rt.storageController().startupKind(), QStringLiteral("none"));
        QVERIFY(rt.storage().isOpen());
#ifdef Q_OS_UNIX
        QVERIFY(QFile::exists(SingleInstance::nameFor(folder)));  // and it now answers a second launch
#endif
    }

    // A socket that can't be created is a visible warning, not just a log line.
    // (A runtime dir path too long for a Unix socket makes listen() fail while
    // the lock file still works.)
    void socketFailureIsReportedToTheUser()
    {
#ifdef Q_OS_WIN
        QSKIP("a too-long Unix socket path is the Unix-only way to make listen() fail");
#endif
        QTemporaryDir dir;
        const QString deep = QDir(dir.path()).filePath(QString(120, QLatin1Char('d')));
        QVERIFY(QDir().mkpath(deep));
        QFile::setPermissions(deep, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        struct Restore {
            QByteArray old = qgetenv("XDG_RUNTIME_DIR");
            ~Restore() { qputenv("XDG_RUNTIME_DIR", old); }
        } restore;
        qputenv("XDG_RUNTIME_DIR", deep.toUtf8());

        AppRuntime::Options o;
        o.noElevation = true;
        o.settingsFile = dir.filePath(QStringLiteral("a.ini"));
        o.dataFolder = dir.filePath(QStringLiteral("data"));
        AppRuntime rt(o);
        QVERIFY(rt.claimInstance());
        QCOMPARE(rt.storageController().startupKind(), QStringLiteral("none"));  // the app still works
        QVERIFY2(!rt.startup().instanceWarning().isEmpty(), "the user must be told a second launch can't be redirected");
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
#if defined(SYNBENCH_EXE) && !defined(Q_OS_WIN)
        // (Windows: SynBench.exe is a GUI-subsystem program with no stderr to read
        // the "listening" / "asked for this window" log lines from, and it asks
        // for administrator rights. The in-process tests above cover the logic.)
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
