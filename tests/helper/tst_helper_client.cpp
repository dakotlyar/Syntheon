// SPDX-License-Identifier: GPL-3.0-or-later
#include "../unit/smbios_builder.h"
#include "privileged/linux_client/linux_helper_client.h"
#include "sysinfo/collector.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <csignal>
#include <unistd.h>

using namespace sb;
using namespace std::chrono_literals;

namespace {

const QString kHelper = QStringLiteral(SB_HELPER_PATH);
const QString kTestHelper = QStringLiteral(SB_TEST_HELPER_PATH);

} // namespace

// LinuxHelperClient end to end, without root: against the real helper running
// as this user, against the test-only helper (fixture SMBIOS), and against
// scripts standing in for pkexec's exit codes and a hung/dying helper.
class TestHelperClient : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_entry, m_table, m_hello;

    QString script(const QString &name, const QString &body)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write("#!/bin/sh\n" + body.toUtf8() + "\n");
            f.close();
        }
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        return path;
    }

    LinuxHelperClient::Launch testHelper(const QString &buildId = QStringLiteral("test"))
    {
        return {{kTestHelper, {m_entry, m_table, buildId}}, std::nullopt};
    }

    LinuxHelperClient::Options options(bool requireRoot = true)
    {
        LinuxHelperClient::Options o;
        o.expectedBuildId = QStringLiteral("test");
        o.requireRoot = requireRoot;
        o.requestTimeout = 5s;
        return o;
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<LinuxHelperClient::State>();
        qRegisterMetaType<LinuxHelperClient::Failure>();
        m_entry = m_dir.filePath(QStringLiteral("smbios_entry_point"));
        m_table = m_dir.filePath(QStringLiteral("DMI"));
        QFile e(m_entry);
        QVERIFY(e.open(QIODevice::WriteOnly));
        e.write(QByteArray("_SM3_\x00\x18\x03\x06", 9));
        e.close();
        QFile t(m_table);
        QVERIFY(t.open(QIODevice::WriteOnly));
        t.write(SmbiosBuilder::sample());
        t.close();
        // A HELLO frame for scripts that pretend to be a helper.
        proto::Frame f;
        f.op = std::uint16_t(proto::Op::Hello);
        f.flags = proto::Response;
        f.payload = proto::encodeHello({proto::kProtocolVersion, 0, "test"});
        const proto::Bytes b = proto::encode(f);
        m_hello = m_dir.filePath(QStringLiteral("hello.bin"));
        QFile h(m_hello);
        QVERIFY(h.open(QIODevice::WriteOnly));
        h.write(reinterpret_cast<const char *>(b.data()), qint64(b.size()));
    }

    // AP5-R2-1: the launch decision trusts nothing from the environment
    // that it can't check.
    void launchCommands()
    {
        QTemporaryDir t;
        const auto exe = [&](const QString &rel) {
            const QString p = t.filePath(rel);
            QDir().mkpath(QFileInfo(p).path());
            QFile f(p);
            if (f.open(QIODevice::WriteOnly))
                f.write("#!/bin/sh\n");
            f.close();
            QFile::setPermissions(p, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            return QFileInfo(p).canonicalFilePath();
        };
        const QString pkexec = exe(QStringLiteral("sys/bin/pkexec"));
        const QString pathPkexec = exe(QStringLiteral("evil/pkexec"));  // a $PATH entry: never used
        Q_UNUSED(pathPkexec)
        const QString app = exe(QStringLiteral("build/bin/Syntheon"));
        const QString helper = exe(QStringLiteral("build/bin/syntheon-helper"));
        const QString image = exe(QStringLiteral("Syntheon-x86_64.AppImage"));
        const QString mountedApp = exe(QStringLiteral("mount_abc/usr/bin/Syntheon"));
        exe(QStringLiteral("mount_abc/usr/bin/syntheon-helper"));

        LinuxHelperClient::Environment dev;
        dev.applicationFilePath = app;
        dev.pkexecDirs = {t.filePath(QStringLiteral("nope")), t.filePath(QStringLiteral("sys/bin"))};
        auto l = LinuxHelperClient::defaultLaunch(dev);
        QVERIFY(l);
        QCOMPARE(l->primary.program, pkexec);  // absolute, from the fixed dirs
        QCOMPARE(l->primary.arguments, QStringList{helper});
        QVERIFY(!l->fallback);

        // A real AppImage: the binary runs from inside $APPDIR.
        LinuxHelperClient::Environment appImage = dev;
        appImage.applicationFilePath = mountedApp;
        appImage.appDir = t.filePath(QStringLiteral("mount_abc"));
        appImage.appImage = image;
        // The kernel's mount table says $APPDIR is a FUSE mount of this image.
        const auto mountTable = [&](const QString &fsType, const QString &source, const QString &mountPoint) {
            static int counter = 0;
            const QString path = t.filePath(QStringLiteral("mountinfo-%1").arg(++counter));
            QFile m(path);
            if (m.open(QIODevice::WriteOnly))
                m.write(QStringLiteral("22 1 0:5 / /proc rw - proc proc rw\n"
                                       "99 22 0:44 / %1 ro,nosuid - %2 %3 ro\n").arg(mountPoint, fsType, source).toUtf8());
            return path;
        };
        appImage.mountInfo = mountTable(QStringLiteral("fuse.AppImage"), image, appImage.appDir);
        l = LinuxHelperClient::defaultLaunch(appImage);
        QVERIFY(l);
        QCOMPARE(l->primary.arguments, (QStringList{image, QStringLiteral("--syntheon-helper")}));
        // No extract-and-run fallback under pkexec: the runtime would extract to a
        // predictable world-readable folder that root then executes from.
        QVERIFY(!l->fallback);

        // AP-5: the environment alone proves nothing. Without a matching FUSE
        // mount of this image at $APPDIR, both variables are ignored.
        for (const QString &bad : {mountTable(QStringLiteral("ext4"), image, appImage.appDir),                        // not FUSE
                                    mountTable(QStringLiteral("fuse.AppImage"), t.filePath(QStringLiteral("other.AppImage")), appImage.appDir),  // another image
                                    mountTable(QStringLiteral("fuse.AppImage"), image, t.filePath(QStringLiteral("elsewhere"))),  // another mount point
                                    t.filePath(QStringLiteral("no-such-mountinfo"))}) {
            LinuxHelperClient::Environment forged = appImage;
            forged.mountInfo = bad;
            QString why;
            const auto launch = LinuxHelperClient::defaultLaunch(forged, &why);
            // Inside an AppImage with an unverifiable mount there is NO launch at all (the
            // dev path would only report a missing helper and hide the real reason), and the
            // error says what failed.
            QVERIFY2(!launch, qPrintable(why));
            QVERIFY2(why.contains(QLatin1StringView("mount could not be verified")), qPrintable(why));
            QVERIFY2(!why.contains(QLatin1StringView("missing")), qPrintable(why));
        }
        // What the real type-2 runtime reports (observed on a real AppImage):
        // fs type "fuse.<file name>", source "<file name>" (no directory).
        const QString name = QFileInfo(image).fileName();
        LinuxHelperClient::Environment realistic = appImage;
        realistic.mountInfo = mountTable(QStringLiteral("fuse.") + name, name, appImage.appDir);
        QCOMPARE(LinuxHelperClient::defaultLaunch(realistic)->primary.arguments.value(0), image);  // the AppImage branch
        // Special characters in the file name: the kernel octal-escapes them in
        // the fs type and the source (observed on a real AppImage named
        // "Syntheon test (1).AppImage": fuse.Syntheon\040test\040(1).AppImage).
        for (const QString &odd : {QStringLiteral("My App (1).AppImage"), QStringLiteral("a#b.AppImage"),
                                    QStringLiteral("back\\slash.AppImage"), QStringLiteral("tab\tname.AppImage")}) {
            const QString oddImage = exe(odd);
            const auto esc = [](QString v) {
                v.replace(QLatin1Char('\\'), QStringLiteral("\\134")).replace(QLatin1Char(' '), QStringLiteral("\\040"));
                v.replace(QLatin1Char('\t'), QStringLiteral("\\011")).replace(QLatin1Char('#'), QStringLiteral("\\043"));
                return v;
            };
            LinuxHelperClient::Environment e = appImage;
            e.appImage = oddImage;
            e.mountInfo = mountTable(QStringLiteral("fuse.") + esc(odd), esc(odd), appImage.appDir);
            const auto launch = LinuxHelperClient::defaultLaunch(e);
            QVERIFY2(launch && launch->primary.arguments.value(0) == oddImage, qPrintable(odd));
        }
        // ...but a name that isn't this image's is refused.
        LinuxHelperClient::Environment otherName = appImage;
        otherName.mountInfo = mountTable(QStringLiteral("fuse.Other.AppImage"), QStringLiteral("Other.AppImage"), appImage.appDir);
        QString otherWhy;
        QVERIFY(!LinuxHelperClient::defaultLaunch(otherName, &otherWhy));
        QVERIFY(otherWhy.contains(QLatin1StringView("expected fs type")));

        // A stray or forged $APPIMAGE is ignored: binary not inside $APPDIR,
        // no $APPDIR, or a relative/non-executable image.
        LinuxHelperClient::Environment stray = appImage;
        stray.applicationFilePath = app;
        QCOMPARE(LinuxHelperClient::defaultLaunch(stray)->primary.arguments, QStringList{helper});
        LinuxHelperClient::Environment noDir = appImage;
        noDir.appDir.clear();
        noDir.applicationFilePath = app;
        QCOMPARE(LinuxHelperClient::defaultLaunch(noDir)->primary.arguments, QStringList{helper});
        LinuxHelperClient::Environment relative = appImage;
        relative.appImage = QStringLiteral("Syntheon-x86_64.AppImage");
        QVERIFY(!LinuxHelperClient::defaultLaunch(relative));  // the binary IS inside $APPDIR: refused, not ignored
        LinuxHelperClient::Environment notExec = appImage;
        notExec.appImage = t.filePath(QStringLiteral("plain-file"));
        QFile pf(notExec.appImage);
        QVERIFY(pf.open(QIODevice::WriteOnly));
        pf.close();
        QVERIFY(!LinuxHelperClient::defaultLaunch(notExec));

        // The real mount table (/proc: size 0, so QFile::atEnd() is true at once): reading it
        // must work. This is the bug that made every real AppImage fail the check while the
        // tests, which read ordinary temp files, passed.
        {
            const auto proc = LinuxHelperClient::verifyAppImageMount(QStringLiteral("/proc/self/mountinfo"), QStringLiteral("/proc"),
                                                                     image);
            QVERIFY2(proc.reason.contains(QLatin1StringView("not FUSE")), qPrintable(proc.reason + " | " + proc.detail));
            QVERIFY2(proc.detail.contains(QLatin1StringView("fs type 'proc'")), qPrintable(proc.detail));
        }
        // The dev path says where it looked.
        {
            LinuxHelperClient::Environment lost = dev;
            lost.applicationFilePath = t.filePath(QStringLiteral("evil/pkexec"));
            QString lostWhy;
            QVERIFY(!LinuxHelperClient::defaultLaunch(lost, &lostWhy));
            QVERIFY2(lostWhy.contains(QLatin1StringView("Looked in")) && lostWhy.contains(QLatin1StringView("libexec")), qPrintable(lostWhy));
        }

        // No pkexec in the fixed dirs: an error, $PATH is never searched.
        LinuxHelperClient::Environment noPkexec = dev;
        noPkexec.pkexecDirs = {t.filePath(QStringLiteral("nope"))};
        QString error;
        QVERIFY(!LinuxHelperClient::defaultLaunch(noPkexec, &error));
        QVERIFY(error.contains(QLatin1StringView("pkexec")));

        // The helper path is canonical (a symlinked app dir resolves).
        QVERIFY(QFile::link(t.filePath(QStringLiteral("build")), t.filePath(QStringLiteral("linkdir"))));
        LinuxHelperClient::Environment linked = dev;
        linked.applicationFilePath = t.filePath(QStringLiteral("linkdir/bin/Syntheon"));
        QCOMPARE(LinuxHelperClient::defaultLaunch(linked)->primary.arguments, QStringList{helper});

        // Helper missing: an error, not a bogus path.
        QFile::remove(helper);
        QVERIFY(!LinuxHelperClient::defaultLaunch(dev, &error));

        QVERIFY(LinuxHelperClient::thisBuildId().contains(QLatin1StringView("linux-x64")));
        QCOMPARE(LinuxHelperClient::defaultPkexecDirs().first(), QStringLiteral("/usr/bin"));
    }

    void smbiosEndToEnd()
    {
        LinuxHelperClient c(options());
        QSignalSpy ready(&c, &LinuxHelperClient::ready);
        c.start(testHelper());
        QVERIFY(ready.wait(10000));
        QVERIFY(c.isAvailable());
        PrivError error;
        QVERIFY(c.ping(&error));

        // The task-10 path: SMBIOS through the helper into sysinfo.
        SystemInfo info;
        QVERIFY(sysinfo::applyPrivilegedSmbios(info, c, &error));
        QCOMPARE(info.memory.modules.size(), 8);  // Knowhere's (SmbiosBuilder::sample)
        QCOMPARE(info.memory.modules[0].type, QStringLiteral("LPDDR5"));
        QCOMPARE(info.memory.channels, 8);
    }

    void requestsFromManyThreads()
    {
        LinuxHelperClient c(options());
        QSignalSpy ready(&c, &LinuxHelperClient::ready);
        c.start(testHelper());
        QVERIFY(ready.wait(10000));
        std::atomic<int> ok{0};
        QList<QThread *> threads;
        for (int i = 0; i < 6; ++i) {
            threads.append(QThread::create([&] {
                for (int j = 0; j < 10; ++j) {
                    if (c.readSmbios(nullptr))
                        ++ok;
                }
            }));
            threads.last()->start();
        }
        for (QThread *t : threads)
            QTRY_VERIFY_WITH_TIMEOUT(t->isFinished(), 20000);
        qDeleteAll(threads);
        QCOMPARE(ok.load(), 60);
    }

    void realHelperAsThisUser()
    {
        if (geteuid() == 0)
            QSKIP("running as root");
        LinuxHelperClient::Options o = options(false);
        o.expectedBuildId = LinuxHelperClient::thisBuildId();
        LinuxHelperClient c(o);
        QSignalSpy ready(&c, &LinuxHelperClient::ready);
        c.start({{kHelper, {}}, std::nullopt});
        QVERIFY(ready.wait(10000));
        PrivError error;
        QVERIFY(c.ping(&error));
        QVERIFY(!c.readSmbios(&error));
        QCOMPARE(error.code, PrivError::Code::NotPermitted);  // not root: EACCES, cleanly
    }

    void realHelperNotRootIsRefused()
    {
        if (geteuid() == 0)
            QSKIP("running as root");
        LinuxHelperClient::Options o = options(true);
        o.expectedBuildId = LinuxHelperClient::thisBuildId();
        LinuxHelperClient c(o);
        QSignalSpy failed(&c, &LinuxHelperClient::failed);
        c.start({{kHelper, {}}, std::nullopt});
        QVERIFY(failed.wait(10000));
        QCOMPARE(c.failure(), LinuxHelperClient::Failure::NotRoot);
    }

    void staleHelperIsRefused()
    {
        LinuxHelperClient c(options());
        QSignalSpy failed(&c, &LinuxHelperClient::failed);
        c.start(testHelper(QStringLiteral("2025.01.0 linux-x64 0000000")));
        QVERIFY(failed.wait(10000));
        QCOMPARE(c.failure(), LinuxHelperClient::Failure::BuildMismatch);
        PrivError error;
        QVERIFY(!c.readSmbios(&error));
        QCOMPARE(error.code, PrivError::Code::Unavailable);
    }

    void pkexecExitCodes_data()
    {
        QTest::addColumn<int>("code");
        QTest::addColumn<int>("failure");
        QTest::newRow("dismissed") << 126 << int(LinuxHelperClient::Failure::Cancelled);
        QTest::newRow("not authorised") << 127 << int(LinuxHelperClient::Failure::NotAuthorised);
        QTest::newRow("other") << 1 << int(LinuxHelperClient::Failure::Exited);
    }

    void pkexecExitCodes()
    {
        QFETCH(int, code);
        QFETCH(int, failure);
        LinuxHelperClient c(options());
        QSignalSpy failed(&c, &LinuxHelperClient::failed);
        c.start({{script(QStringLiteral("exit%1.sh").arg(code), QStringLiteral("exit %1").arg(code)), {}}, std::nullopt});
        QVERIFY(failed.wait(10000));
        QCOMPARE(int(c.failure()), failure);
        QVERIFY(!c.failureMessage().isEmpty());

        // Retry (D1) starts it again from scratch.
        QSignalSpy ready(&c, &LinuxHelperClient::ready);
        c.start(testHelper());
        QVERIFY(ready.wait(10000));
        QCOMPARE(c.failure(), LinuxHelperClient::Failure::None);
    }

    // (e) The helper's own exit code 126/127 isn't pkexec's "dismissed" /
    // "not authorised"; only pkexec's own words (or silence) are.
    void exitCodeClassification_data()
    {
        QTest::addColumn<QString>("body");
        QTest::addColumn<int>("failure");
        const int c = int(LinuxHelperClient::Failure::Cancelled), n = int(LinuxHelperClient::Failure::NotAuthorised),
                  e = int(LinuxHelperClient::Failure::Exited);
        QTest::newRow("pkexec: dismissed") << "echo 'Error executing command as another user: Request dismissed' >&2; exit 126" << c;
        QTest::newRow("pkexec: not authorized") << "echo 'Error executing command as another user: Not authorized' >&2; exit 127" << n;
        QTest::newRow("silent 126") << "exit 126" << c;
        QTest::newRow("silent 127") << "exit 127" << n;
        QTest::newRow("AppImage can't exec (126)") << "echo 'Syntheon.AppImage: Permission denied' >&2; exit 126" << e;
        QTest::newRow("AppImage not found (127)") << "echo 'exec: No such file or directory' >&2; exit 127" << e;
        QTest::newRow("helper started, then 126") << "echo 'syntheon-helper: started (euid 0) for uid 1000' >&2; exit 126" << e;
        QTest::newRow("helper started, then 127") << "echo 'syntheon-helper: started (euid 0) for uid 1000' >&2; exit 127" << e;
    }

    void exitCodeClassification()
    {
        QFETCH(QString, body);
        QFETCH(int, failure);
        LinuxHelperClient c(options());
        QSignalSpy failed(&c, &LinuxHelperClient::failed);
        c.start({{script(QStringLiteral("classify.sh"), body), {}}, std::nullopt});
        QVERIFY(failed.wait(10000));
        QCOMPARE(int(c.failure()), failure);
    }

    // The unanswered prompt times out (Dave: 2 minutes; short here), pkexec
    // is stopped, and Retry starts a fresh prompt.
    void authorisationTimesOut()
    {
        LinuxHelperClient::Options o = options();
        o.authTimeout = 400ms;
        LinuxHelperClient c(o);
        QSignalSpy failed(&c, &LinuxHelperClient::failed);
        QElapsedTimer t;
        t.start();
        c.start({{script(QStringLiteral("waiting.sh"), QStringLiteral("exec sleep 60")), {}}, std::nullopt});
        const qint64 pid = c.helperProcessId();
        QCOMPARE(c.state(), LinuxHelperClient::State::Authorising);
        QVERIFY(failed.wait(10000));
        QCOMPARE(c.failure(), LinuxHelperClient::Failure::TimedOut);
        QVERIFY(c.failureMessage().contains(QLatin1StringView("timed out")));
        QVERIFY2(t.elapsed() >= 350 && t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
        // The pending pkexec process is gone.
        QTRY_VERIFY_WITH_TIMEOUT(::kill(pid_t(pid), 0) != 0, 8000);

        QSignalSpy ready(&c, &LinuxHelperClient::ready);
        c.start(testHelper());
        QVERIFY(ready.wait(10000));
        QCOMPARE(c.failure(), LinuxHelperClient::Failure::None);
        // And a prompt that is answered in time doesn't time out later.
        QTest::qWait(600);
        QCOMPARE(c.state(), LinuxHelperClient::State::Ready);
    }

    void launchFailure()

    {
        LinuxHelperClient c(options());
        QSignalSpy failed(&c, &LinuxHelperClient::failed);
        c.start({{m_dir.filePath(QStringLiteral("no-such-pkexec")), {}}, std::nullopt});
        QVERIFY(failed.wait(10000));
        QCOMPARE(c.failure(), LinuxHelperClient::Failure::LaunchFailed);
    }

    void appImageFallbackRetriesOnce()
    {
        // Stands in for the AppImage: fails unless asked to extract-and-run.
        const QString fake = script(QStringLiteral("fake-appimage.sh"),
                                    QStringLiteral("if [ \"$1\" = \"--appimage-extract-and-run\" ]; then exec '%1' '%2' '%3' test; fi\nexit 1")
                                        .arg(kTestHelper, m_entry, m_table));
        LinuxHelperClient c(options());
        QSignalSpy ready(&c, &LinuxHelperClient::ready);
        c.start({{fake, {QStringLiteral("--syntheon-helper")}},
                 LinuxHelperClient::Command{fake, {QStringLiteral("--appimage-extract-and-run"), QStringLiteral("--syntheon-helper")}}});
        QVERIFY(ready.wait(10000));
        QVERIFY(c.readSmbios(nullptr));

        // Only once: if the fallback fails too, that's the error.
        const QString broken = script(QStringLiteral("broken.sh"), QStringLiteral("exit 1"));
        LinuxHelperClient d(options());
        QSignalSpy failed(&d, &LinuxHelperClient::failed);
        d.start({{broken, {}}, LinuxHelperClient::Command{broken, {}}});
        QVERIFY(failed.wait(10000));
        QCOMPARE(d.failure(), LinuxHelperClient::Failure::Exited);
    }

    void requestTimesOut()
    {
        // Says HELLO, then never answers.
        const QString mute = script(QStringLiteral("mute.sh"), QStringLiteral("cat '%1'\nexec sleep 30").arg(m_hello));
        LinuxHelperClient::Options o = options();
        o.requestTimeout = 300ms;
        LinuxHelperClient c(o);
        QSignalSpy ready(&c, &LinuxHelperClient::ready);
        c.start({{mute, {}}, std::nullopt});
        QVERIFY(ready.wait(10000));
        PrivError error;
        QVERIFY(!c.readSmbios(&error));  // on the client's own thread
        QCOMPARE(error.code, PrivError::Code::IoError);
        std::optional<SmbiosTables> fromWorker;
        PrivError workerError;
        QThread *t = QThread::create([&] { fromWorker = c.readSmbios(&workerError); });
        t->start();
        QTRY_VERIFY_WITH_TIMEOUT(t->isFinished(), 5000);
        delete t;
        QVERIFY(!fromWorker);
        QCOMPARE(workerError.code, PrivError::Code::IoError);
        ::kill(pid_t(c.helperProcessId()), SIGKILL);
    }

    void helperDeathFailsInFlightRequests()
    {
        const QString mute = script(QStringLiteral("mute2.sh"), QStringLiteral("cat '%1'\nexec sleep 30").arg(m_hello));
        LinuxHelperClient::Options o = options();
        o.requestTimeout = 60s;  // long: only the death can end the wait
        LinuxHelperClient c(o);
        QSignalSpy ready(&c, &LinuxHelperClient::ready);
        QSignalSpy failed(&c, &LinuxHelperClient::failed);
        c.start({{mute, {}}, std::nullopt});
        QVERIFY(ready.wait(10000));

        PrivError workerError;
        std::atomic<bool> done{false};
        QThread *t = QThread::create([&] {
            c.readSmbios(&workerError);
            done = true;
        });
        t->start();
        QTest::qWait(200);
        QVERIFY(!done);
        ::kill(pid_t(c.helperProcessId()), SIGKILL);
        QVERIFY(failed.wait(5000));
        QTRY_VERIFY_WITH_TIMEOUT(done.load(), 5000);  // never a hang
        t->wait();
        delete t;
        QCOMPARE(workerError.code, PrivError::Code::Unavailable);
        QCOMPARE(c.failure(), LinuxHelperClient::Failure::Exited);
        QCOMPARE(c.state(), LinuxHelperClient::State::Failed);
    }

    // AP5-R2-5: requests racing the helper's death all end promptly with
    // Unavailable; none waits out its (long) timeout.
    void deathRacesManyRequests()
    {
        LinuxHelperClient::Options o = options();
        o.requestTimeout = 60s;
        LinuxHelperClient c(o);
        QSignalSpy ready(&c, &LinuxHelperClient::ready);
        c.start(testHelper());
        QVERIFY(ready.wait(10000));
        std::atomic<bool> stopFlag{false};
        std::atomic<int> unavailable{0};
        QList<QThread *> threads;
        for (int i = 0; i < 8; ++i) {
            threads.append(QThread::create([&] {
                while (!stopFlag) {
                    PrivError e;
                    if (!c.readSmbios(&e) && e.code == PrivError::Code::Unavailable) {
                        ++unavailable;
                        return;
                    }
                }
            }));
            threads.last()->start();
        }
        QTest::qWait(100);
        ::kill(pid_t(c.helperProcessId()), SIGKILL);
        QElapsedTimer t;
        t.start();
        for (QThread *th : threads)
            QTRY_VERIFY_WITH_TIMEOUT(th->isFinished(), 5000);
        QVERIFY(t.elapsed() < 5000);
        stopFlag = true;
        qDeleteAll(threads);
        QCOMPARE(unavailable.load(), 8);
        QCOMPARE(c.state(), LinuxHelperClient::State::Failed);
    }

    void garbageInsteadOfHello()
    {
        const QString noisy = script(QStringLiteral("noisy.sh"), QStringLiteral("printf 'hello there, not a frame'\nexec sleep 30"));
        LinuxHelperClient c(options());
        QSignalSpy failed(&c, &LinuxHelperClient::failed);
        c.start({{noisy, {}}, std::nullopt});
        QVERIFY(failed.wait(10000));
        QCOMPARE(c.failure(), LinuxHelperClient::Failure::BadHello);
        ::kill(pid_t(c.helperProcessId()), SIGKILL);
    }
};

QTEST_GUILESS_MAIN(TestHelperClient)
#include "tst_helper_client.moc"
