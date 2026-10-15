// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <memory>

namespace sb {

// D8: system sleep is blocked for every run, from Start until the run row is
// finalised. The screen may still turn off (Battery blocks that itself in
// Phase 8). If inhibiting fails, the run logs a warning and carries on.
//
// acquire() and release() are called on the run worker thread, because the
// Windows mechanism (SetThreadExecutionState) is per-thread. release() is
// idempotent.
//
// F7: when a run is abandoned (a test ignored its cancel, R7), the
// orchestrator calls release() from its own thread. That lifts the logind
// lock on Linux, but on Windows only the hung worker thread can lift its
// state, which it does if and when the test finally returns (or the process
// exits). So on Windows the sleep block can outlive an abandoned run.
// The inhibitor must outlive the run's worker thread (it's an app-lifetime
// object).
class PowerInhibitor
{
public:
    virtual ~PowerInhibitor() = default;
    virtual bool acquire(const QString &why, QString *error) = 0;
    virtual void release() = 0;
};

// The platform's inhibitor: logind over D-Bus on Linux,
// SetThreadExecutionState on Windows. Defined in synbench_power.
std::unique_ptr<PowerInhibitor> createSystemPowerInhibitor();

} // namespace sb
