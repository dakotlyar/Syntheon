// SPDX-License-Identifier: GPL-3.0-or-later
#include "../unit/smbios_builder.h"
#include "privileged/linux_client/linux_helper_client.h"
#include "sysinfo/collector.h"

#include <QFile>
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

    void launchCommands()
    {
        const auto app = LinuxHelperClient::defaultLaunch(QStringLiteral("/home/u/SynBench.AppImage"), QStringLiteral("/x"));
        QCOMPARE(app.primary.program, QStringLiteral("pkexec"));
        QCOMPARE(app.primary.arguments,
                 (QStringList{QStringLiteral("/home/u/SynBench.AppImage"), QStringLiteral("--synbench-helper")}));
        QVERIFY(app.fallback);
        QCOMPARE(app.fallback->arguments,
                 (QStringList{QStringLiteral("/home/u/SynBench.AppImage"), QStringLiteral("--appimage-extract-and-run"),
                              QStringLiteral("--synbench-helper")}));
        const auto dev = LinuxHelperClient::defaultLaunch({}, QStringLiteral("/opt/build/bin"));
        QCOMPARE(dev.primary.arguments, QStringList{QStringLiteral("/opt/build/bin/synbench-helper")});
        QVERIFY(!dev.fallback);
        QVERIFY(LinuxHelperClient::thisBuildId().contains(QLatin1StringView("linux-x64")));
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
        QCOMPARE(info.memory.modules.size(), 2);
        QCOMPARE(info.memory.modules[0].type, QStringLiteral("LPDDR5"));
        QCOMPARE(info.memory.channels, 2);
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
        c.start({{fake, {QStringLiteral("--synbench-helper")}},
                 LinuxHelperClient::Command{fake, {QStringLiteral("--appimage-extract-and-run"), QStringLiteral("--synbench-helper")}}});
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
