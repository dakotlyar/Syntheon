// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/power_inhibitor.h"

#include <QMutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sb {

namespace {

// SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED): the state is
// per-thread, so release() only takes effect on the thread that acquired. A
// thread's state is also dropped when that thread exits.
class WindowsPowerInhibitor : public PowerInhibitor
{
public:
    ~WindowsPowerInhibitor() override { release(); }

    bool acquire(const QString &, QString *error) override
    {
        QMutexLocker lock(&m_mutex);
        if (m_held)
            return true;
        if (SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) == 0) {
            if (error)
                *error = QStringLiteral("SetThreadExecutionState failed (error %1)").arg(static_cast<qulonglong>(GetLastError()));
            return false;
        }
        m_held = true;
        m_thread = GetCurrentThreadId();
        return true;
    }

    void release() override
    {
        QMutexLocker lock(&m_mutex);
        // Only the acquiring thread can clear its execution state. A release
        // from another thread (an abandoned run) leaves the lock "held" so the
        // owning thread's own release() still clears it when it returns.
        if (!m_held || GetCurrentThreadId() != m_thread)
            return;
        SetThreadExecutionState(ES_CONTINUOUS);
        m_held = false;
    }

private:
    QMutex m_mutex;
    bool m_held = false;
    DWORD m_thread = 0;
};

} // namespace

std::unique_ptr<PowerInhibitor> createSystemPowerInhibitor()
{
    return std::make_unique<WindowsPowerInhibitor>();
}

} // namespace sb
