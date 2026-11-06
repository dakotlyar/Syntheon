// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "privileged/privileged_ops.h"
#include "sysinfo/system_info.h"

#include <QObject>
#include <QTimer>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

#ifdef Q_OS_LINUX
#include "privileged/linux_client/linux_helper_client.h"
#endif

namespace sb {

// App-level startup state (§6, §7): the administrator gate (D1) and the
// system snapshot. QML singleton "AppController".
//
// Linux: starts the privileged helper (one polkit prompt). While it starts,
// authState is Authorising; if the prompt is declined, the helper fails, or it
// dies mid-session, authState is Failed and QML shows the blocking AuthDialog
// (Retry / Quit; there's no limited mode). Windows: the app is already
// elevated (P12), so authState is Ready at once.
//
// The snapshot is collected on a pool thread; memory modules come from SMBIOS
// through PrivilegedOps once both are ready (Linux: the helper's READ_SMBIOS).
class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(AuthState authState READ authState NOTIFY authChanged)
    Q_PROPERTY(QString authMessage READ authMessage NOTIFY authChanged)
    Q_PROPERTY(QString authStateName READ authStateName NOTIFY authChanged)
    Q_PROPERTY(bool systemInfoReady READ systemInfoReady NOTIFY systemInfoChanged)
    Q_PROPERTY(QString machineLabel READ machineLabel NOTIFY systemInfoChanged)
    Q_PROPERTY(QString systemSummary READ systemSummary NOTIFY systemInfoChanged)
    Q_PROPERTY(bool memoryModulesKnown READ memoryModulesKnown NOTIFY systemInfoChanged)
    Q_PROPERTY(QStringList memoryModules READ memoryModules NOTIFY systemInfoChanged)
    // System info screen: [{label, value}], API pills [{text, tone}], battery lines.
    Q_PROPERTY(QVariantList systemRows READ systemRows NOTIFY systemInfoChanged)
    Q_PROPERTY(QVariantList apiPills READ apiPills NOTIFY systemInfoChanged)
    Q_PROPERTY(QStringList batteryLines READ batteryLines NOTIFY powerChanged)
    Q_PROPERTY(QString section READ section WRITE setSection NOTIFY sectionChanged)
    Q_PROPERTY(QString headerMachine READ headerMachine NOTIFY systemInfoChanged)
    Q_PROPERTY(QString headerDetail READ headerDetail NOTIFY powerChanged)
    Q_PROPERTY(bool emulated READ emulated NOTIFY systemInfoChanged)
    Q_PROPERTY(QString emulationText READ emulationText NOTIFY systemInfoChanged)
    Q_PROPERTY(QString version READ version CONSTANT)
    // THIRD-PARTY-NOTICES.txt (D11), embedded in the app: Settings > Licences.
    Q_PROPERTY(QString thirdPartyNotices READ thirdPartyNotices CONSTANT)

public:
    enum class AuthState { Authorising, Ready, Failed };
    Q_ENUM(AuthState)

#ifdef Q_OS_LINUX
    // Tests replace the pkexec launch; the app uses LinuxHelperClient's default.
    using LaunchProvider = std::function<LinuxHelperClient::Launch()>;
    explicit AppController(LaunchProvider launch = {}, LinuxHelperClient::Options options = defaultHelperOptions(),
                           QObject *parent = nullptr);
    static LinuxHelperClient::Options defaultHelperOptions();
#else
    explicit AppController(QObject *parent = nullptr);
#endif
    ~AppController() override;

    void start();
    // Smoke runs (SYNTHEON_SMOKE=1, CI under xvfb) have no polkit agent: no
    // helper is started, the gate is open and privileged calls are
    // Unavailable. Never set in a normal launch.
    void setElevationDisabled(bool disabled) { m_noElevation = disabled; }

    AuthState authState() const { return m_authState; }
    QString authMessage() const { return m_authMessage; }
    // "authorising" | "ready" | "failed" (QML compares strings, not enums).
    QString authStateName() const;
    bool systemInfoReady() const { return m_infoReady; }
    QString machineLabel() const { return m_info.machineLabel(); }
    QString systemSummary() const;
    bool memoryModulesKnown() const { return m_info.memory.modulesKnown; }
    QStringList memoryModules() const;
    QVariantList systemRows() const;
    QVariantList apiPills() const;
    QStringList batteryLines() const;

    const SystemInfo &systemInfo() const { return m_info; }
#ifdef SYNTHEON_DEV_HOOKS
    // Test hook, compiled out of packaged builds: pretend this snapshot was
    // collected (the real one is read on a pool thread at start).
    void setSystemInfoForTest(const SystemInfo &info)
    {
        m_info = info;
        m_infoReady = true;  // no signals: the run controller isn't probing in a unit test
    }
    // Test hook: report this power state (present, AC, charge) in place of
    // the machine's, from the start-up snapshot and every 5 s poll alike, so
    // a test is the same on AC, on battery or without one. Only the input is
    // faked: preconditions and the header are computed from it as usual.
    // nullopt goes back to the real readings (read again at once).
    void setPowerForTest(std::optional<BatteryInfo> power);
#endif
    PrivilegedOps &privileged();
    // Fails every pending privileged request at once (a blocked probe or
    // SMBIOS read returns immediately). For shutdown; the helper is closed.
    void abortPrivileged();

    // The visible section: run | progress | results | history | compare |
    // system | settings. Results and its Detail sub-page share "results".
    QString section() const { return m_section; }
    void setSection(const QString &section);
    QString headerMachine() const;
    QString headerDetail() const;  // "Windows 11 x64 · on AC power"
    bool emulated() const { return m_info.emulated; }
    QString emulationText() const;
    QString version() const;
    QString thirdPartyNotices() const;
    // Fresh power state, polled every 5 s while the app runs.
    void refreshPower();
    // The current snapshot with the latest power state, for a run to freeze.
    SystemSnapshot snapshotForRun() const { return m_info.toSnapshot(); }

    Q_INVOKABLE void retryAuth();
    Q_INVOKABLE void quit();
    // Exit with code 1 and `message` on stderr (the "may already be open" dialog's Quit).
    Q_INVOKABLE void quitFailed(const QString &message);

signals:
    void authChanged();
    void systemInfoChanged();
    void quitRequested();
    void sectionChanged();
    void powerChanged();

private:
    void setAuth(AuthState state, const QString &message = {});
    void startHelper();
    void readSmbiosWhenReady();
    void onSmbios(std::optional<SmbiosTables> tables, PrivError error);

#ifdef Q_OS_LINUX
    LaunchProvider m_launch;
    LinuxHelperClient m_helper;
#else
    std::unique_ptr<PrivilegedOps> m_ops;
#endif
    AuthState m_authState = AuthState::Authorising;
    QString m_authMessage;
    SystemInfo m_info;
    bool m_infoReady = false;
    bool m_smbiosRequested = false;
    QString m_section = QStringLiteral("run");
    QTimer m_powerTimer;
    bool m_powerBusy = false;
#ifdef SYNTHEON_DEV_HOOKS
    std::optional<BatteryInfo> m_powerForTest;
#endif
    void applyPower(BatteryInfo read);
    bool m_noElevation = false;
    UnavailablePrivilegedOps m_unavailable;
};

} // namespace sb
