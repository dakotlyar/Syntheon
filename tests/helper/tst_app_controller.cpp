// SPDX-License-Identifier: GPL-3.0-or-later
#include "../unit/smbios_builder.h"
#include "app/controllers/app_controller.h"

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
        const QStringList modules = c.memoryModules();
        QCOMPARE(modules.size(), 2);
        QCOMPARE(modules[0], QStringLiteral("DIMM 0: 32 GB LPDDR5 8000 MT/s Samsung K3KL3L30EM-BGCV"));
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

        c.retryAuth();  // Retry: prompts again
        QCOMPARE(c.authState(), AppController::AuthState::Authorising);
        QTRY_COMPARE_WITH_TIMEOUT(c.authState(), AppController::AuthState::Ready, 10000);
        QCOMPARE(launches, 2);
        QTRY_VERIFY_WITH_TIMEOUT(c.memoryModulesKnown(), 20000);

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
