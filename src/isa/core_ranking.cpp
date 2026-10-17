// SPDX-License-Identifier: GPL-3.0-or-later
#include "isa/core_ranking.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
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

// A sysfs CPU list ("0-3,8,10-11").
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

} // namespace

CoreChoice rankLinuxCores(const std::string &sysfsRoot, const std::vector<int> &allowed)
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

    // The first source any of these CPUs has decides; the highest value wins,
    // then the lowest CPU number (SMT siblings share a core's ranking).
    static const char *const sources[][2] = {
        {"acpi_cppc/highest_perf", "acpi_cppc/highest_perf"},
        {"cpu_capacity", "cpu_capacity"},
        {"cpufreq/cpuinfo_max_freq", "cpuinfo_max_freq"},
    };
    for (const auto &source : sources) {
        CoreChoice best;
        long long bestValue = -1;
        for (int cpu : cpus) {
            const auto v = readNumber(base / ("cpu" + std::to_string(cpu)) / source[0]);
            if (v && *v > bestValue) {
                bestValue = *v;
                best.ranked = true;
                best.cpu = cpu;
                best.source = source[1];
            }
        }
        if (best.ranked)
            return best;
    }
    return {};
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

CoreChoice bestCoreForSingleThread()
{
#if defined(__linux__)
    std::vector<int> allowed;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int c = 0; c < CPU_SETSIZE; ++c) {
            if (CPU_ISSET(c, &set))
                allowed.push_back(c);
        }
    }
    return rankLinuxCores("/sys", allowed);
#elif defined(_WIN32)
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
    return rankWindowsCpuSets(records);
#else
    return {};
#endif
}

} // namespace sb::isa
