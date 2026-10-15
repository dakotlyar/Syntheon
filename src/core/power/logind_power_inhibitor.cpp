// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/power/logind_power_inhibitor.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>

namespace sb {

LogindPowerInhibitor::LogindPowerInhibitor() = default;

LogindPowerInhibitor::~LogindPowerInhibitor()
{
    release();
}

bool LogindPowerInhibitor::acquire(const QString &why, QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (m_fd)
        return true;

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        if (error)
            *error = QStringLiteral("no system D-Bus");
        return false;
    }
    QDBusInterface manager(QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
                           QStringLiteral("org.freedesktop.login1.Manager"), bus);
    if (!manager.isValid()) {
        if (error)
            *error = QStringLiteral("logind isn't available: %1").arg(manager.lastError().message());
        return false;
    }
    const QDBusReply<QDBusUnixFileDescriptor> reply =
        manager.call(QStringLiteral("Inhibit"), QStringLiteral("sleep"), QStringLiteral("SynBench"), why,
                     QStringLiteral("block"));
    if (!reply.isValid() || !reply.value().isValid()) {
        if (error)
            *error = QStringLiteral("logind refused the sleep lock: %1").arg(reply.error().message());
        return false;
    }
    m_fd = std::make_unique<QDBusUnixFileDescriptor>(reply.value());
    return true;
}

void LogindPowerInhibitor::release()
{
    QMutexLocker lock(&m_mutex);
    m_fd.reset();  // closes our fd; logind drops the lock
}

bool LogindPowerInhibitor::isHeld() const
{
    QMutexLocker lock(&m_mutex);
    return m_fd != nullptr;
}

std::unique_ptr<PowerInhibitor> createSystemPowerInhibitor()
{
    return std::make_unique<LogindPowerInhibitor>();
}

} // namespace sb
