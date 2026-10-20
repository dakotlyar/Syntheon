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
//
// Several cores often tie at the top (Knowhere: CPUs 0, 5, 12 and 17 at
// highest_perf 216). Decision D (2026-10-17): before each single-thread
// repetition the engine samples idle time for 250 ms and pins to the tied
// core whose physical core (both SMT threads, or the core itself without SMT)
// was idle the most, so a background program on one of them doesn't share
// the core with the measurement. The tie set and the choice are pure
// functions too; only the samples are live.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
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

// --- The top tie set and the quietest core in it (decision D) ---

// A core tied at the top of the ranking, and the logical CPUs of its
// physical core (itself included), as keys into an IdleSample.
struct CoreCandidate {
    CoreChoice core;
    std::vector<int> smtKeys;
};

// Cumulative idle time per logical CPU, by key. Linux: CPU number -> idle +
// iowait jiffies (/proc/stat); Windows: windowsKey(group, index) -> idle
// cycles (QueryIdleProcessorCycleTimeEx). Only differences between two
// samples mean anything.
using IdleSample = std::map<int, std::uint64_t>;
constexpr int windowsKey(int group, int logicalIndex) { return group * 64 + logicalIndex; }

// Linux: every CPU tied at the top of rankLinuxCores' ranking (same source,
// same value), lowest number first, each with its SMT siblings from
// cpuN/topology/thread_siblings_list (just itself when that file is missing).
// Empty when there is no ranking. The first one is rankLinuxCores' choice.
std::vector<CoreCandidate> linuxTopCandidates(const std::string &sysfsRoot, const std::vector<int> &allowed);
// The idle column sums of /proc/stat's "cpuN" lines (fields 4 and 5: idle
// and iowait).
IdleSample parseProcStatIdle(const std::string &procStat);

// Windows: every CPU set tied at the top of rankWindowsCpuSets' ranking
// (unparked, EfficiencyClass, SchedulingClass), lowest group and index first,
// so a hybrid CPU stays on its P-cores. `cores` lists each physical core's
// logical processors as windowsKey()s (GetLogicalProcessorInformationEx,
// RelationProcessorCore); a processor in none of them is a core of its own.
// The first one is rankWindowsCpuSets' choice.
std::vector<CoreCandidate> windowsTopCandidates(const std::vector<CpuSetRecord> &records,
                                                const std::vector<std::vector<int>> &cores);

// The candidate whose physical core was idle the most between the two
// samples: the mean over its SMT keys, so cores with and without SMT compare
// fairly (a key missing from either sample counts as never idle). Ties: the
// earlier candidate. Unranked if there are none.
CoreChoice quietestCandidate(const std::vector<CoreCandidate> &candidates, const IdleSample &before,
                             const IdleSample &after);

// A sysfs CPU list ("0-3,8,10-11"), as in thread_siblings_list and
// shared_cpu_list.
std::set<int> parseCpuList(const std::string &text);

// This machine: the platform's tie set (empty: no ranking), and an idle
// sample of every logical CPU (empty where the platform has none).
std::vector<CoreCandidate> topCandidatesForSingleThread();
IdleSample sampleIdle();

// --- Every logical CPU (Phase 3 §4.2, P11, P12) ----------------------------

// The IdleSample key of a CPU: windowsKey(group, cpu), which on Linux (group
// 0) is the CPU number. The keys of CoreCandidate::smtKeys.
constexpr int cpuKey(const CoreChoice &c) { return windowsKey(c.group, c.cpu); }

// Every logical CPU this process may run on, each as a ranked CoreChoice to
// pin to (source "affinity"), in order: Linux by CPU number
// (sched_getaffinity); Windows by group, then index (CPU sets, skipping those
// allocated to another process, real-time ones and ones outside the process
// affinity, as the ranking does). Empty where the platform has neither.
std::vector<CoreChoice> allowedLogicalCpus();

} // namespace sb::isa
