// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/controllers/app_controller.h"

#include "sysinfo/collector.h"

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
    // Pool tasks capture `this`; let them finish first.
    QThreadPool::globalInstance()->waitForDone();
}

PrivilegedOps &AppController::privileged()
{
#ifdef Q_OS_LINUX
    return m_helper;
#else
    return *m_ops;
#endif
}

void AppController::setAuth(AuthState state, const QString &message)
{
    m_authState = state;
    m_authMessage = message;
    emit authChanged();
}

void AppController::start()
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
    QThreadPool::globalInstance()->start([this] {
        const SystemInfo info = sysinfo::collectSystemInfo();
        QMetaObject::invokeMethod(this, [this, info] {
            m_info = info;
            sysinfo::addOpenGlInfo(m_info);  // GUI thread
            m_infoReady = true;
            qCInfo(lcApp) << "system info collected for" << m_info.machineLabel();
            emit systemInfoChanged();
            readSmbiosWhenReady();
        });
    });
}

void AppController::retryAuth()
{
#ifdef Q_OS_LINUX
    if (m_authState != AuthState::Failed)
        return;
    setAuth(AuthState::Authorising);
    if (m_launch)
        m_helper.start(m_launch());
    else
        m_helper.start();
#endif
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
