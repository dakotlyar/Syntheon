// SPDX-License-Identifier: GPL-3.0-or-later
#include "../unit/smbios_builder.h"
#include "app/controllers/app_controller.h"
#include "app/controllers/run_controller.h"
#include "app/controllers/storage_controller.h"
#include "storage/settings_store.h"
#include "storage/storage.h"
#include "sysinfo/parsers/smbios.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>

using namespace sb;
using namespace std::chrono_literals;

// The administrator gate (D1) and the SMBIOS wiring, with the pkexec launch
// replaced by the test helper (fixture SMBIOS) or by scripts standing in for
// pkexec's "dismissed" exit. Nothing here prompts or runs as root.
class TestAppController : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_entry, m_table;

    LinuxHelperClient::Launch testHelper() const
    {
        return {{QStringLiteral(SB_TEST_HELPER_PATH), {m_entry, m_table, QStringLiteral("test")}}, std::nullopt};
    }
    LinuxHelperClient::Launch dismissed()
    {
        const QString path = m_dir.filePath(QStringLiteral("pkexec-dismissed.sh"));
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write("#!/bin/sh\nexit 126\n");
        f.close();
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        return {{path, {}}, std::nullopt};
    }
    static LinuxHelperClient::Options options()
    {
        LinuxHelperClient::Options o;
        o.expectedBuildId = QStringLiteral("test");
        o.requireRoot = true;
        o.requestTimeout = 5s;
        return o;
    }

private slots:
    void initTestCase()
    {
        m_entry = m_dir.filePath(QStringLiteral("ep"));
        m_table = m_dir.filePath(QStringLiteral("dmi"));
        QFile e(m_entry);
        QVERIFY(e.open(QIODevice::WriteOnly));
        e.write(QByteArray("_SM3_\x00\x18\x03\x06", 9));
        e.close();
        QFile t(m_table);
        QVERIFY(t.open(QIODevice::WriteOnly));
        t.write(SmbiosBuilder::sample());
    }

    void acceptedPromptShowsMemoryModules()
    {
        AppController c([this] { return testHelper(); }, options());
        c.start();
        QCOMPARE(c.authState(), AppController::AuthState::Authorising);
        QTRY_COMPARE_WITH_TIMEOUT(c.authState(), AppController::AuthState::Ready, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(c.systemInfoReady(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(c.memoryModulesKnown(), 10000);
        // The modules may first be udev's copy (the collector's baseline,
        // Phase 3 D11): wait for the helper's tables to replace them.
        QTRY_COMPARE_WITH_TIMEOUT(c.memoryModules().value(0),
                                  QStringLiteral("DIMM 0: 4 GB LPDDR5 8,000 MT/s 1R 32-bit Micron Technology MT62F1G32D2DS-023 WT"), 10000);
        QCOMPARE(c.memoryModules().size(), 8);
        QVERIFY(!c.systemSummary().isEmpty());
        QVERIFY(c.privileged().isAvailable());
    }

    void declinedPromptThenRetryAndQuit()
    {
        int launches = 0;
        AppController c([&] { return ++launches == 1 ? dismissed() : testHelper(); }, options());
        QSignalSpy quit(&c, &AppController::quitRequested);
        c.start();
        QTRY_COMPARE_WITH_TIMEOUT(c.authState(), AppController::AuthState::Failed, 10000);
        QVERIFY(c.authMessage().contains(QLatin1StringView("cancelled")));
        QVERIFY(!c.privileged().isAvailable());
        // Phase 3 D11: declined, the modules are the collector's udev
        // baseline where this machine has one (read-only; a CI runner may not).
        QFile udev(QStringLiteral("/run/udev/data/+dmi:id"));
        const bool haveUdev = udev.open(QIODevice::ReadOnly)
                              && !sysinfo::parseUdevMemoryDevices(QString::fromUtf8(udev.readAll())).isEmpty();
        if (haveUdev)
            QTRY_VERIFY_WITH_TIMEOUT(c.memoryModulesKnown(), 20000);

        c.retryAuth();  // Retry: prompts again
        QCOMPARE(c.authState(), AppController::AuthState::Authorising);
        QTRY_COMPARE_WITH_TIMEOUT(c.authState(), AppController::AuthState::Ready, 10000);
        QCOMPARE(launches, 2);
        QTRY_VERIFY_WITH_TIMEOUT(c.memoryModulesKnown(), 20000);
        // ...and the helper's tables replace udev's once it is up.
        QTRY_COMPARE_WITH_TIMEOUT(c.memoryModules().value(0),
                                  QStringLiteral("DIMM 0: 4 GB LPDDR5 8,000 MT/s 1R 32-bit Micron Technology MT62F1G32D2DS-023 WT"), 20000);

        c.quit();  // Quit
        QCOMPARE(quit.count(), 1);
    }

    void retryOnlyWhenFailed()
    {
        int launches = 0;
        AppController c([&] {
            ++launches;
            return testHelper();
        }, options());
        c.start();
        QTRY_COMPARE_WITH_TIMEOUT(c.authState(), AppController::AuthState::Ready, 10000);
        c.retryAuth();
        QCOMPARE(launches, 1);
    }

    // AP5-R2-6: destroying the controller while a pool task waits on a
    // silent helper must not freeze for the request timeout.
    void destructionDoesntWaitForRequests()
    {
        const QString mute = m_dir.filePath(QStringLiteral("mute.sh"));
        proto::Frame f;
        f.op = std::uint16_t(proto::Op::Hello);
        f.flags = proto::Response;
        f.payload = proto::encodeHello({proto::kProtocolVersion, 0, "test"});
        const proto::Bytes hello = proto::encode(f);
        const QString helloFile = m_dir.filePath(QStringLiteral("hello.bin"));
        QFile h(helloFile);
        QVERIFY(h.open(QIODevice::WriteOnly));
        h.write(reinterpret_cast<const char *>(hello.data()), qint64(hello.size()));
        h.close();
        QFile s(mute);
        QVERIFY(s.open(QIODevice::WriteOnly));
        s.write(("#!/bin/sh\ncat '" + helloFile + "'\nexec sleep 60\n").toUtf8());
        s.close();
        QFile::setPermissions(mute, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        LinuxHelperClient::Options o = options();
        o.requestTimeout = 60s;
        QElapsedTimer t;
        {
            AppController c([mute] { return LinuxHelperClient::Launch{{mute, {}}, std::nullopt}; }, o);
            c.start();
            QTRY_COMPARE_WITH_TIMEOUT(c.authState(), AppController::AuthState::Ready, 10000);
            QTRY_VERIFY_WITH_TIMEOUT(c.systemInfoReady(), 20000);
            QTest::qWait(300);  // the SMBIOS request is now waiting on the mute helper
            auto &helper = static_cast<LinuxHelperClient &>(c.privileged());
            const pid_t pid = pid_t(helper.helperProcessId());
            t.start();
            // destructor runs here
            QMetaObject::invokeMethod(&c, [] {});
            Q_UNUSED(pid)
        }
        QVERIFY2(t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
    }

    // Quitting while a probe waits on a silent helper: the run controller's
    // destructor must fail the request first (it used to wait the full
    // request timeout for the global pool).
    void runControllerDestructionDoesntWaitForRequests()
    {
        const QString mute = m_dir.filePath(QStringLiteral("mute2.sh"));
        proto::Frame f;
        f.op = std::uint16_t(proto::Op::Hello);
        f.flags = proto::Response;
        f.payload = proto::encodeHello({proto::kProtocolVersion, 0, "test"});
        const proto::Bytes hello = proto::encode(f);
        const QString helloFile = m_dir.filePath(QStringLiteral("hello2.bin"));
        QFile h(helloFile);
        QVERIFY(h.open(QIODevice::WriteOnly));
        h.write(reinterpret_cast<const char *>(hello.data()), qint64(hello.size()));
        h.close();
        QFile s(mute);
        QVERIFY(s.open(QIODevice::WriteOnly));
        s.write(("#!/bin/sh\ncat '" + helloFile + "'\nexec sleep 60\n").toUtf8());
        s.close();
        QFile::setPermissions(mute, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        LinuxHelperClient::Options o = options();
        o.requestTimeout = 60s;
        QElapsedTimer t;
        {
            AppController c([mute] { return LinuxHelperClient::Launch{{mute, {}}, std::nullopt}; }, o);
            SettingsStore store(m_dir.filePath(QStringLiteral("rc.ini")));
            store.setDataLocation(m_dir.filePath(QStringLiteral("rcdata")));
            QDir().mkpath(m_dir.filePath(QStringLiteral("rcdata")));
            Storage storage(store);
            StorageController sc(storage, store);
            {
                RunController rc(storage, sc, c);
                c.start();
                QTRY_COMPARE_WITH_TIMEOUT(c.authState(), AppController::AuthState::Ready, 10000);
                QTRY_VERIFY_WITH_TIMEOUT(c.systemInfoReady(), 20000);
                QTest::qWait(300);  // the SMBIOS request is waiting on the mute helper
                t.start();
            }  // ~RunController
            QVERIFY2(t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
        }
        QVERIFY2(t.elapsed() < 8000, qPrintable(QString::number(t.elapsed())));
    }

    void helperDyingMidSessionBlocksAgain()
    {
        AppController c([this] { return testHelper(); }, options());
        c.start();
        QTRY_COMPARE_WITH_TIMEOUT(c.authState(), AppController::AuthState::Ready, 10000);
        auto &helper = static_cast<LinuxHelperClient &>(c.privileged());
        ::kill(pid_t(helper.helperProcessId()), SIGKILL);
        QTRY_COMPARE_WITH_TIMEOUT(c.authState(), AppController::AuthState::Failed, 10000);
        QVERIFY(c.authMessage().contains(QLatin1StringView("stopped")));
    }
};

QTEST_MAIN(TestAppController)
#include "tst_app_controller.moc"
