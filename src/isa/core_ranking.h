// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which logical CPU the single-thread mark runs on (Phase 2 design §4.3, P4):
// the top-ranked core by the firmware's / OS's own ranking, so it runs on the
// same, fastest core every time. Plain C++20, no Qt.
//
//   Windows  GetSystemCpuSetInformation: the highest EfficiencyClass, then the
//            highest SchedulingClass (the preferred-core ranking), then the
//            lowest group / index
//   Linux    acpi_cppc/highest_perf (AMD and Intel preferred cores), else
//            cpu_capacity (ARM), else cpufreq/cpuinfo_max_freq: the highest
//            value, then the lowest CPU number; only CPUs this process may
//            run on (sched_getaffinity: taskset, cgroups)
//   neither  no ranking: the caller runs unpinned
//
// The rankers are pure functions over a sysfs tree / CPU-set records, so the
// tests feed them fixtures and mocked data.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sb::isa {

struct CoreChoice {
    bool ranked = false;     // false: no ranking data; run unpinned
    int cpu = -1;            // Linux: the CPU number; Windows: the logical index within `group`
    int group = 0;           // Windows processor group (0 on Linux)
    std::uint32_t cpuSetId = 0;  // Windows CPU set id (0 on Linux)
    std::string source;      // what ranked it: "acpi_cppc/highest_perf", "cpu_capacity",
                             // "cpuinfo_max_freq", "EfficiencyClass/SchedulingClass"
};

// Linux: rank the CPUs under <sysfsRoot>/devices/system/cpu (normally
// sysfsRoot = "/sys") among `allowed` (CPU numbers; empty = every CPU found).
CoreChoice rankLinuxCores(const std::string &sysfsRoot, const std::vector<int> &allowed);

// Windows: one record per logical processor, as GetSystemCpuSetInformation
// returns them, plus whether the process's affinity allows it. Skipped: CPU
// sets allocated to another process, real-time ones, ones outside the
// process affinity. Unparked ones win over parked ones, then the ranking.
struct CpuSetRecord {
    std::uint32_t id = 0;
    int group = 0;
    int logicalIndex = 0;  // LogicalProcessorIndex within the group
    int efficiencyClass = 0;
    int schedulingClass = 0;
    bool allocatedElsewhere = false;  // reserved for another process (Allocated && !AllocatedToTargetProcess)
    bool realTime = false;            // reserved for real-time work: skipped
    bool parked = false;              // parked by power management right now: only if nothing else
    bool inProcessAffinity = true;    // the process's affinity allows it: skipped otherwise
};
CoreChoice rankWindowsCpuSets(const std::vector<CpuSetRecord> &records);

// This machine: the platform's ranker on the live data.
CoreChoice bestCoreForSingleThread();

} // namespace sb::isa
