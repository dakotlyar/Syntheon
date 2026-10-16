// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/controllers/app_controller.h"

#include "sysinfo/collector.h"

#include "synbench_version.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QThreadPool>

#ifdef Q_OS_WIN
#include "privileged/win_inproc/windows_inprocess_ops.h"
#endif

Q_LOGGING_CATEGORY(lcApp, "synbench.app")

namespace sb {

#ifdef Q_OS_LINUX
LinuxHelperClient::Options AppController::defaultHelperOptions()
{
    LinuxHelperClient::Options o;
    o.expectedBuildId = LinuxHelperClient::thisBuildId();
    o.requireRoot = true;
    return o;
}

AppController::AppController(LaunchProvider launch, LinuxHelperClient::Options options, QObject *parent)
    : QObject(parent), m_launch(std::move(launch)), m_helper(std::move(options))
{
    connect(&m_helper, &LinuxHelperClient::ready, this, [this] {
        setAuth(AuthState::Ready);
        readSmbiosWhenReady();
    });
    connect(&m_helper, &LinuxHelperClient::failed, this,
            [this](LinuxHelperClient::Failure, const QString &message) {
                m_smbiosRequested = false;  // try again after a successful Retry
                setAuth(AuthState::Failed, message);
            });
}
#else
AppController::AppController(QObject *parent)
    : QObject(parent), m_ops(std::make_unique<WindowsInProcessOps>())
{
}
#endif

AppController::~AppController()
{
#ifdef Q_OS_LINUX
    // AP5-R2-6: a pool task may be blocked in a privileged request (up to the
    // 15 s timeout). Closing the helper fails every pending request at once,
    // so the wait below ends promptly instead of freezing the window.
    m_helper.shutdown();
#endif
    // Pool tasks capture `this`; let them finish first.
    QThreadPool::globalInstance()->waitForDone();
}

PrivilegedOps &AppController::privileged()
{
    if (m_noElevation)
        return m_unavailable;
#ifdef Q_OS_LINUX
    return m_helper;
#else
    return *m_ops;
#endif
}

QString AppController::authStateName() const
{
    switch (m_authState) {
    case AuthState::Authorising: return QStringLiteral("authorising");
    case AuthState::Ready: return QStringLiteral("ready");
    case AuthState::Failed: return QStringLiteral("failed");
    }
    return {};
}

void AppController::setAuth(AuthState state, const QString &message)
{
    m_authState = state;
    m_authMessage = message;
    emit authChanged();
}

void AppController::setSection(const QString &section)
{
    if (section == m_section)
        return;
    m_section = section;
    emit sectionChanged();
}

QString AppController::headerMachine() const
{
    return m_infoReady ? m_info.machineLabel() : QString();
}

QString AppController::headerDetail() const
{
    if (!m_infoReady)
        return {};
    QStringList parts;
    parts << QStringLiteral("%1 %2").arg(m_info.os.name, m_info.os.arch).trimmed();
    if (m_info.battery.present)
        parts << (m_info.battery.acOnline ? QStringLiteral("on AC power")
                                          : QStringLiteral("on battery · %1%").arg(qRound(std::max(0.0, m_info.battery.chargePct))));
    else
        parts << QStringLiteral("no battery");
    return parts.join(QStringLiteral(" · "));
}

QString AppController::emulationText() const
{
    if (!m_info.emulated)
        return {};
    return QStringLiteral("This is the x64 build running emulated on an ARM64 PC, so results are not native. "
                          "Install the ARM64 build for real numbers.");
}

QString AppController::version() const
{
    return m_info.app.version.isEmpty() ? QStringLiteral(SYNBENCH_DISPLAY_VERSION) : m_info.app.version;
}

void AppController::refreshPower()
{
    if (m_powerBusy || !m_infoReady)
        return;
    m_powerBusy = true;
    QThreadPool::globalInstance()->start([this] {
        const BatteryInfo b = sysinfo::readBatteryNow();
        QMetaObject::invokeMethod(this, [this, b] {
            m_powerBusy = false;
            const bool changed = b.present != m_info.battery.present || b.acOnline != m_info.battery.acOnline
                                 || qRound(b.chargePct) != qRound(m_info.battery.chargePct);
            // Keep the one-off fields (design capacity, health, cycles) from the full snapshot.
            m_info.battery.present = b.present;
            m_info.battery.acOnline = b.acOnline;
            m_info.battery.chargePct = b.chargePct;
            if (changed)
                emit powerChanged();
        });
    });
}

void AppController::startHelper()
{
#ifdef Q_OS_LINUX
    setAuth(AuthState::Authorising);
    if (m_launch)
        m_helper.start(m_launch());
    else
        m_helper.start();
#else
    setAuth(AuthState::Ready);  // already elevated (P12)
#endif
}

void AppController::start()
{
    m_powerTimer.setInterval(5000);
    connect(&m_powerTimer, &QTimer::timeout, this, &AppController::refreshPower);
    m_powerTimer.start();

    if (m_noElevation) {
        setAuth(AuthState::Ready);
        m_smbiosRequested = true;  // nothing to ask for
    } else {
        startHelper();
    }

    QThreadPool::globalInstance()->start([this] {
        const SystemInfo info = sysinfo::collectSystemInfo();
        QMetaObject::invokeMethod(this, [this, info] {
            m_info = info;
            sysinfo::addOpenGlInfo(m_info);  // GUI thread
            m_infoReady = true;
            qCInfo(lcApp) << "system info collected for" << m_info.machineLabel();
            emit systemInfoChanged();
            emit powerChanged();
            readSmbiosWhenReady();
        });
    });
}

void AppController::retryAuth()
{
    if (m_authState != AuthState::Failed)
        return;
    startHelper();
}

void AppController::quit()
{
    emit quitRequested();
}

void AppController::readSmbiosWhenReady()
{
    if (!m_infoReady || m_smbiosRequested || !privileged().isAvailable())
        return;
#ifdef Q_OS_WIN
    if (m_info.memory.modulesKnown)
        return;  // the Windows collector already read SMBIOS in-process
#endif
    m_smbiosRequested = true;
    // Blocking request: off the GUI thread.
    QThreadPool::globalInstance()->start([this] {
        PrivError error;
        auto tables = privileged().readSmbios(&error);
        QMetaObject::invokeMethod(this, [this, tables, error] { onSmbios(tables, error); });
    });
}

void AppController::onSmbios(std::optional<SmbiosTables> tables, PrivError error)
{
    if (!tables) {
        // An ordinary error with a reason (D1: nothing is "needs admin").
        qCWarning(lcApp) << "couldn't read SMBIOS:" << error.message;
        return;
    }
    const auto smbios = sysinfo::parseSmbiosTables(*tables);
    if (!smbios) {
        qCWarning(lcApp) << "unrecognised SMBIOS entry point";
        return;
    }
    sysinfo::applySmbios(m_info, *smbios);
    qCInfo(lcApp) << "memory modules:" << m_info.memory.modules.size();
    emit systemInfoChanged();
}

QString AppController::systemSummary() const
{
    if (!m_infoReady)
        return {};
    const double gib = double(m_info.memory.totalBytes) / (1024.0 * 1024 * 1024);
    return QStringLiteral("%1 · %2 · %3 GB RAM · %4")
        .arg(m_info.machineLabel(), m_info.cpu.name)
        .arg(gib, 0, 'f', 1)
        .arg(m_info.os.name);
}

QStringList AppController::memoryModules() const
{
    QStringList out;
    for (const MemoryModule &m : m_info.memory.modules) {
        QStringList parts;
        if (!m.locator.isEmpty())
            parts << m.locator + QLatin1Char(':');
        parts << QStringLiteral("%1 GB").arg(double(m.sizeBytes) / (1024.0 * 1024 * 1024), 0, 'f', 0);
        if (!m.type.isEmpty())
            parts << m.type;
        if (m.configuredSpeedMts > 0 || m.speedMts > 0)
            parts << QStringLiteral("%1 MT/s").arg(m.configuredSpeedMts > 0 ? m.configuredSpeedMts : m.speedMts);
        if (!m.manufacturer.isEmpty())
            parts << m.manufacturer;
        if (!m.partNumber.isEmpty())
            parts << m.partNumber;
        out << parts.join(u' ');
    }
    return out;
}

} // namespace sb
