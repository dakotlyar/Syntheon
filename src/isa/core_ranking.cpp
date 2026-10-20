// SPDX-License-Identifier: GPL-3.0-or-later
#include "isa/core_ranking.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <tuple>

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace sb::isa {

namespace {

// The first line of a sysfs file as a non-negative integer.
std::optional<long long> readNumber(const std::filesystem::path &file)
{
    std::ifstream in(file);
    std::string line;
    if (!in || !std::getline(in, line))
        return std::nullopt;
    errno = 0;
    char *end = nullptr;
    const long long v = std::strtoll(line.c_str(), &end, 10);
    if (end == line.c_str() || errno != 0 || v < 0)
        return std::nullopt;
    return v;
}


#if defined(__linux__)
// The CPUs this process may run on (sched_getaffinity: taskset, cgroups).
std::vector<int> linuxAllowedCpus()
{
    std::vector<int> allowed;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int c = 0; c < CPU_SETSIZE; ++c) {
            if (CPU_ISSET(c, &set))
                allowed.push_back(c);
        }
    }
    return allowed;
}
#endif

// The CPUs under <sysfsRoot>/devices/system/cpu tied at the top of the
// ranking, lowest number first, and the source that ranked them.
std::vector<int> linuxTopTies(const std::string &sysfsRoot, const std::vector<int> &allowed, std::string &source)
{
    namespace fs = std::filesystem;
    const fs::path base = fs::path(sysfsRoot) / "devices" / "system" / "cpu";
    std::error_code ec;
    std::set<int> cpus;
    for (const fs::directory_entry &e : fs::directory_iterator(base, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() > 3 && name.compare(0, 3, "cpu") == 0
            && std::all_of(name.begin() + 3, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            cpus.insert(std::atoi(name.c_str() + 3));
    }
    // Online CPUs only, when the kernel says which.
    if (std::ifstream online(base / "online"); online) {
        std::string line;
        if (std::getline(online, line)) {
            const std::set<int> on = parseCpuList(line);
            if (!on.empty()) {
                std::set<int> both;
                std::set_intersection(cpus.begin(), cpus.end(), on.begin(), on.end(), std::inserter(both, both.begin()));
                cpus = both;
            }
        }
    }
    if (!allowed.empty()) {
        const std::set<int> mask(allowed.begin(), allowed.end());
        std::set<int> both;
        std::set_intersection(cpus.begin(), cpus.end(), mask.begin(), mask.end(), std::inserter(both, both.begin()));
        cpus = both;
    }

    // The first source any of these CPUs has decides; the highest value wins
    // (SMT siblings share a core's ranking).
    static const char *const sources[][2] = {
        {"acpi_cppc/highest_perf", "acpi_cppc/highest_perf"},
        {"cpu_capacity", "cpu_capacity"},
        {"cpufreq/cpuinfo_max_freq", "cpuinfo_max_freq"},
    };
    for (const auto &s : sources) {
        std::vector<int> top;
        long long bestValue = -1;
        for (int cpu : cpus) {
            const auto v = readNumber(base / ("cpu" + std::to_string(cpu)) / s[0]);
            if (!v || *v < bestValue)
                continue;
            if (*v > bestValue) {
                bestValue = *v;
                top.clear();
            }
            top.push_back(cpu);
        }
        if (!top.empty()) {
            source = s[1];
            return top;
        }
    }
    return {};
}

} // namespace

std::set<int> parseCpuList(const std::string &text)
{
    std::set<int> cpus;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t end = text.find(',', i);
        if (end == std::string::npos)
            end = text.size();
        const std::string item = text.substr(i, end - i);
        const std::size_t dash = item.find('-');
        char *e = nullptr;
        const long first = std::strtol(item.c_str(), &e, 10);
        if (e != item.c_str()) {
            long last = first;
            if (dash != std::string::npos)
                last = std::strtol(item.c_str() + dash + 1, &e, 10);
            for (long c = first; c <= last && c - first < 65536; ++c)
                cpus.insert(int(c));
        }
        i = end + 1;
    }
    return cpus;
}

CoreChoice rankLinuxCores(const std::string &sysfsRoot, const std::vector<int> &allowed)
{
    // The lowest-numbered of the top CPUs.
    const std::vector<CoreCandidate> top = linuxTopCandidates(sysfsRoot, allowed);
    return top.empty() ? CoreChoice{} : top.front().core;
}

std::vector<CoreCandidate> linuxTopCandidates(const std::string &sysfsRoot, const std::vector<int> &allowed)
{
    namespace fs = std::filesystem;
    std::string source;
    std::vector<CoreCandidate> out;
    for (const int cpu : linuxTopTies(sysfsRoot, allowed, source)) {
        CoreCandidate c;
        c.core.ranked = true;
        c.core.cpu = cpu;
        c.core.source = source;
        c.smtKeys = linuxSmtSiblingKeys(sysfsRoot, cpu);
        out.push_back(std::move(c));
    }
    return out;
}

IdleSample parseProcStatIdle(const std::string &procStat)
{
    IdleSample idle;
    std::istringstream in(procStat);
    std::string line;
    while (std::getline(in, line)) {
        // "cpuN user nice system idle iowait irq softirq steal ..." (the
        // aggregate "cpu " line has no number: skipped).
        if (line.size() < 4 || line.compare(0, 3, "cpu") != 0 || line[3] < '0' || line[3] > '9')
            continue;
        std::istringstream fields(line.substr(3));
        int cpu = -1;
        unsigned long long user = 0, nice = 0, system = 0, idleTicks = 0, iowait = 0;
        if (fields >> cpu >> user >> nice >> system >> idleTicks) {
            if (!(fields >> iowait))
                iowait = 0;
            idle[cpu] = idleTicks + iowait;
        }
    }
    return idle;
}

CoreChoice rankWindowsCpuSets(const std::vector<CpuSetRecord> &records)
{
    const CpuSetRecord *best = nullptr;
    for (const CpuSetRecord &r : records) {
        if (r.allocatedElsewhere || r.realTime || !r.inProcessAffinity)
            continue;
        if (!best
            || std::make_tuple(!r.parked, r.efficiencyClass, r.schedulingClass, -r.group, -r.logicalIndex)
                   > std::make_tuple(!best->parked, best->efficiencyClass, best->schedulingClass, -best->group,
                                     -best->logicalIndex))
            best = &r;
    }
    if (!best)
        return {};
    CoreChoice c;
    c.ranked = true;
    c.cpu = best->logicalIndex;
    c.group = best->group;
    c.cpuSetId = best->id;
    c.source = "EfficiencyClass/SchedulingClass";
    return c;
}

std::vector<CoreCandidate> windowsTopCandidates(const std::vector<CpuSetRecord> &records,
                                                const std::vector<std::vector<int>> &cores)
{
    const CoreChoice best = rankWindowsCpuSets(records);
    if (!best.ranked)
        return {};
    const CpuSetRecord *top = nullptr;
    for (const CpuSetRecord &r : records) {
        if (!top && r.id == best.cpuSetId && r.group == best.group && r.logicalIndex == best.cpu)
            top = &r;
    }
    std::vector<const CpuSetRecord *> tied;
    for (const CpuSetRecord &r : records) {
        if (r.allocatedElsewhere || r.realTime || !r.inProcessAffinity)
            continue;
        if (r.parked == top->parked && r.efficiencyClass == top->efficiencyClass
            && r.schedulingClass == top->schedulingClass)
            tied.push_back(&r);
    }
    std::sort(tied.begin(), tied.end(), [](const CpuSetRecord *a, const CpuSetRecord *b) {
        return std::make_pair(a->group, a->logicalIndex) < std::make_pair(b->group, b->logicalIndex);
    });
    std::vector<CoreCandidate> out;
    for (const CpuSetRecord *r : tied) {
        CoreCandidate c;
        c.core.ranked = true;
        c.core.cpu = r->logicalIndex;
        c.core.group = r->group;
        c.core.cpuSetId = r->id;
        c.core.source = best.source;
        const int key = windowsKey(r->group, r->logicalIndex);
        c.smtKeys = {key};
        for (const std::vector<int> &core : cores) {
            if (std::find(core.begin(), core.end(), key) != core.end()) {
                c.smtKeys = core;
                std::sort(c.smtKeys.begin(), c.smtKeys.end());
                break;
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

CoreChoice quietestCandidate(const std::vector<CoreCandidate> &candidates, const IdleSample &before,
                             const IdleSample &after)
{
    const CoreCandidate *best = nullptr;
    double bestIdle = 0;
    for (const CoreCandidate &c : candidates) {
        std::uint64_t idle = 0;
        for (const int key : c.smtKeys) {
            const auto b = before.find(key);
            const auto a = after.find(key);
            if (b != before.end() && a != after.end() && a->second > b->second)
                idle += a->second - b->second;
        }
        // The mean per logical CPU, so a 2-thread core isn't favoured over a
        // 1-thread core in a mixed tie set just for having two.
        const double mean = c.smtKeys.empty() ? 0.0 : double(idle) / double(c.smtKeys.size());
        if (!best || mean > bestIdle) {
            best = &c;
            bestIdle = mean;
        }
    }
    return best ? best->core : CoreChoice{};
}

#if defined(_WIN32)
namespace {

// GetSystemCpuSetInformation for this process, as CpuSetRecords.
std::vector<CpuSetRecord> windowsCpuSetRecords()
{
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length == 0)
        return {};
    std::vector<unsigned char> buffer(length);
    auto *first = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data());
    if (!GetSystemCpuSetInformation(first, length, &length, GetCurrentProcess(), 0))
        return {};
    // The process affinity: one group's mask (GetProcessAffinityMask gives
    // zeros when the process spans groups; then nothing is filtered).
    DWORD_PTR processMask = 0, systemMask = 0;
    USHORT groupCount = 1, group = 0;
    const bool haveAffinity = GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) && processMask != 0
                              && GetProcessGroupAffinity(GetCurrentProcess(), &groupCount, &group) && groupCount == 1;
    std::vector<CpuSetRecord> records;
    for (ULONG offset = 0; offset < length;) {
        const auto *info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION *>(buffer.data() + offset);
        if (info->Size == 0)
            break;
        if (info->Type == CpuSetInformation) {
            CpuSetRecord r;
            r.id = info->CpuSet.Id;
            r.group = info->CpuSet.Group;
            r.logicalIndex = info->CpuSet.LogicalProcessorIndex;
            r.efficiencyClass = info->CpuSet.EfficiencyClass;
            r.schedulingClass = info->CpuSet.SchedulingClass;
            r.allocatedElsewhere = info->CpuSet.Allocated && !info->CpuSet.AllocatedToTargetProcess;
            r.realTime = info->CpuSet.RealTime;
            r.parked = info->CpuSet.Parked;
            if (haveAffinity)
                r.inProcessAffinity = r.group == group && r.logicalIndex < int(sizeof(DWORD_PTR) * 8)
                                      && ((processMask >> r.logicalIndex) & 1u);
            records.push_back(r);
        }
        offset += info->Size;
    }
    return records;
}

// GetLogicalProcessorInformationEx(RelationProcessorCore): each physical
// core's logical processors, as windowsKey()s.
std::vector<std::vector<int>> windowsPhysicalCores()
{
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0)
        return {};
    std::vector<unsigned char> buffer(length);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                                          &length))
        return {};
    std::vector<std::vector<int>> cores;
    for (DWORD offset = 0; offset < length;) {
        const auto *info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + offset);
        if (info->Size == 0)
            break;
        if (info->Relationship == RelationProcessorCore) {
            std::vector<int> keys;
            for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
                const GROUP_AFFINITY &ga = info->Processor.GroupMask[g];
                for (int bit = 0; bit < int(sizeof(KAFFINITY) * 8); ++bit) {
                    if ((ga.Mask >> bit) & 1u)
                        keys.push_back(windowsKey(ga.Group, bit));
                }
            }
            cores.push_back(std::move(keys));
        }
        offset += info->Size;
    }
    return cores;
}

} // namespace
#endif

std::vector<int> linuxSmtSiblingKeys(const std::string &sysfsRoot, int cpu)
{
    std::ifstream in(std::filesystem::path(sysfsRoot) / "devices" / "system" / "cpu" / ("cpu" + std::to_string(cpu)) / "topology"
                     / "thread_siblings_list");
    std::string line;
    std::set<int> siblings;
    if (in && std::getline(in, line))
        siblings = parseCpuList(line);
    siblings.insert(cpu);
    return {siblings.begin(), siblings.end()};
}

std::vector<int> smtSiblingKeys(const CoreChoice &core)
{
    if (!core.ranked)
        return {};
#if defined(__linux__)
    return linuxSmtSiblingKeys("/sys", core.cpu);
#elif defined(_WIN32)
    for (const std::vector<int> &keys : windowsPhysicalCores()) {
        if (std::find(keys.begin(), keys.end(), cpuKey(core)) != keys.end())
            return keys;
    }
    return {cpuKey(core)};
#else
    return {cpuKey(core)};
#endif
}

std::vector<CoreCandidate> topCandidatesForSingleThread()
{
#if defined(__linux__)
    return linuxTopCandidates("/sys", linuxAllowedCpus());
#elif defined(_WIN32)
    return windowsTopCandidates(windowsCpuSetRecords(), windowsPhysicalCores());
#else
    return {};
#endif
}

IdleSample sampleIdle()
{
#if defined(__linux__)
    std::ifstream in("/proc/stat");
    std::ostringstream text;
    text << in.rdbuf();
    return parseProcStatIdle(text.str());
#elif defined(_WIN32)
    IdleSample idle;
    const WORD groups = GetActiveProcessorGroupCount();
    for (WORD g = 0; g < groups; ++g) {
        std::vector<ULONG64> cycles(64);
        ULONG size = ULONG(cycles.size() * sizeof(ULONG64));
        if (!QueryIdleProcessorCycleTimeEx(g, &size, cycles.data()))
            continue;
        for (ULONG i = 0; i < size / sizeof(ULONG64); ++i)
            idle[windowsKey(g, int(i))] = cycles[i];
    }
    return idle;
#else
    return {};
#endif
}

std::vector<CoreChoice> allowedLogicalCpus()
{
    std::vector<CoreChoice> cpus;
#if defined(__linux__)
    for (const int c : linuxAllowedCpus())
        cpus.push_back({true, c, 0, 0, "affinity"});
#elif defined(_WIN32)
    for (const CpuSetRecord &r : windowsCpuSetRecords()) {
        if (r.allocatedElsewhere || r.realTime || !r.inProcessAffinity)
            continue;
        cpus.push_back({true, r.logicalIndex, r.group, r.id, "affinity"});
    }
    std::sort(cpus.begin(), cpus.end(), [](const CoreChoice &a, const CoreChoice &b) {
        return std::make_pair(a.group, a.cpu) < std::make_pair(b.group, b.cpu);
    });
#endif
    return cpus;
}

CoreChoice bestCoreForSingleThread()
{
#if defined(__linux__)
    return rankLinuxCores("/sys", linuxAllowedCpus());
#elif defined(_WIN32)
    return rankWindowsCpuSets(windowsCpuSetRecords());
#else
    return {};
#endif
}

} // namespace sb::isa
