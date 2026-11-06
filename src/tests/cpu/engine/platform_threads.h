// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The thread-placement pieces of the measurement engine (Phase 2 design
// §4.1, §4.3, §4.4): how many logical CPUs the test uses, pinning a thread
// to the single-thread core, spreading threads over Windows processor groups,
// and Windows' power throttling (EcoQoS) opt-out. No-ops where the platform
// has nothing to do.

#include "isa/core_ranking.h"

namespace sb::cpu {

// The logical CPUs this process may use: the sched_getaffinity count on Linux
// (it respects taskset and cgroups), GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)
// on Windows. At least 1.
int logicalCpuCount();

// Pins the calling thread to the chosen CPU (pthread_setaffinity_np /
// SetThreadGroupAffinity). False if the choice is unranked or the call failed;
// the thread then runs unpinned.
bool pinCurrentThread(const isa::CoreChoice &core);
// The CPU the calling thread runs on right now (-1 if unknown); for tests.
int currentCpu();
// The calling thread's OS id (Linux: the tid of /proc/self/task/<tid>;
// Windows: GetCurrentThreadId); for tests and the log.
long currentThreadId();

// Windows with more than one processor group (> 64 logical CPUs): worker
// `index` of `count` gets a whole group, in proportion to the groups' sizes,
// because Windows 10 doesn't spread one process's threads across groups by
// itself (§4.3). Group level only: the OS still places the thread inside the
// group (the mask is the group's active processors). Elsewhere, and with one
// group, does nothing. False if the call failed.
bool spreadOverProcessorGroups(int index, int count);

// Windows (P5): the calling thread asks not to be power-throttled (EcoQoS),
// so an unfocused Syntheon isn't moved onto E-cores. Elsewhere, nothing.
void optOutOfPowerThrottlingForThisThread();

// Windows: the calling thread runs at THREAD_PRIORITY_HIGHEST while this
// object lives, then at its old priority. For the engine's coordinator: it
// sleeps almost all the time, but when its 50 ms poll or a worker's "done"
// wakes it, it must run at once, also when every CPU is busy with
// normal-priority threads (its own workers included), which on Windows can
// otherwise hold it off for a whole quantum (about 190 ms on Windows Server)
// and break the 250 ms Stop contract. Elsewhere, nothing: raising a thread's
// priority on Linux needs privileges.
class ScopedRaisedThreadPriority
{
public:
    ScopedRaisedThreadPriority();
    ~ScopedRaisedThreadPriority();
    ScopedRaisedThreadPriority(const ScopedRaisedThreadPriority &) = delete;
    ScopedRaisedThreadPriority &operator=(const ScopedRaisedThreadPriority &) = delete;

private:
    // Used on Windows only.
    [[maybe_unused]] int m_old = 0;
    [[maybe_unused]] bool m_raised = false;
};

// Windows (P5): the process opts out of power throttling while this object
// lives, then goes back to the system's default. Elsewhere, nothing.
class ScopedProcessPowerThrottlingOptOut
{
public:
    ScopedProcessPowerThrottlingOptOut();
    ~ScopedProcessPowerThrottlingOptOut();
    ScopedProcessPowerThrottlingOptOut(const ScopedProcessPowerThrottlingOptOut &) = delete;
    ScopedProcessPowerThrottlingOptOut &operator=(const ScopedProcessPowerThrottlingOptOut &) = delete;
    bool active() const { return m_active; }

private:
    bool m_active = false;
};

} // namespace sb::cpu
