// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/power_inhibitor.h"

#include <QMutex>

#include <memory>

class QDBusUnixFileDescriptor;

namespace sb {

// logind Inhibit("sleep", "SynBench", why, "block") over the system bus. The
// lock lasts as long as the returned fd stays open.
class LogindPowerInhibitor : public PowerInhibitor
{
public:
    LogindPowerInhibitor();
    ~LogindPowerInhibitor() override;

    bool acquire(const QString &why, QString *error) override;
    void release() override;
    bool isHeld() const;

private:
    mutable QMutex m_mutex;
    std::unique_ptr<QDBusUnixFileDescriptor> m_fd;
};

} // namespace sb
