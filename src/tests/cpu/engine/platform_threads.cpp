// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/engine/platform_threads.h"

#include <algorithm>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace sb::cpu {

int logicalCpuCount()
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0)
        return std::max(1, CPU_COUNT(&set));
#elif defined(_WIN32)
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (n > 0)
        return int(n);
#endif
    return std::max(1, int(std::thread::hardware_concurrency()));
}

bool pinCurrentThread(const isa::CoreChoice &core)
{
    if (!core.ranked || core.cpu < 0)
        return false;
#if defined(__linux__)
    if (core.cpu >= CPU_SETSIZE)
        return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core.cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
    if (core.cpu >= int(sizeof(KAFFINITY) * 8))
        return false;
    GROUP_AFFINITY affinity = {};
    affinity.Group = WORD(core.group);
    affinity.Mask = KAFFINITY(1) << core.cpu;
    return SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != 0;
#else
    return false;
#endif
}

int currentCpu()
{
#if defined(__linux__)
    return sched_getcpu();
#elif defined(_WIN32)
    PROCESSOR_NUMBER n;
    GetCurrentProcessorNumberEx(&n);
    return int(n.Number);
#else
    return -1;
#endif
}

void spreadOverProcessorGroups(int index, int count)
{
#if defined(_WIN32)
    const WORD groups = GetActiveProcessorGroupCount();
    if (groups <= 1 || count <= 0)
        return;
    // Worker `index` goes to the group that holds its share: groups get
    // workers in proportion to their active processors.
    const DWORD total = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const unsigned long long position = (unsigned long long)index * total / unsigned(count);
    unsigned long long start = 0;
    for (WORD g = 0; g < groups; ++g) {
        const DWORD size = GetActiveProcessorCount(g);
        if (position < start + size) {
            GROUP_AFFINITY affinity = {};
            affinity.Group = g;
            affinity.Mask = size >= sizeof(KAFFINITY) * 8 ? ~KAFFINITY(0) : ((KAFFINITY(1) << size) - 1);
            SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr);
            return;
        }
        start += size;
    }
#else
    (void)index;
    (void)count;
#endif
}

void optOutOfPowerThrottlingForThisThread()
{
#if defined(_WIN32)
    THREAD_POWER_THROTTLING_STATE state = {};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;  // controlled, and off: never throttled
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
#endif
}

ScopedProcessPowerThrottlingOptOut::ScopedProcessPowerThrottlingOptOut()
{
#if defined(_WIN32)
    PROCESS_POWER_THROTTLING_STATE state = {};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    m_active = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) != 0;
#endif
}

ScopedProcessPowerThrottlingOptOut::~ScopedProcessPowerThrottlingOptOut()
{
#if defined(_WIN32)
    if (!m_active)
        return;
    // Neither bit controlled: the system decides again (the default).
    PROCESS_POWER_THROTTLING_STATE state = {};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
#endif
}

} // namespace sb::cpu
