// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The context the CPU test records next to its numbers, never enforcing it
// (Phase 2 design §4.4): the background load in the second before the run,
// the power profile, and the free memory probe() warns about. Plain C++20.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace sb::cpu {

// Busy and total CPU time, all CPUs (Linux: /proc/stat's "cpu" line, in
// jiffies; Windows: GetSystemTimes, in 100 ns).
struct CpuTimes {
    std::uint64_t busy = 0, total = 0;
};
std::optional<CpuTimes> systemCpuTimes();
// Linux: the aggregate "cpu " line of /proc/stat (busy = everything but idle
// and iowait).
std::optional<CpuTimes> parseProcStatTotal(const std::string &procStat);
// The busy share between two readings, in percent.
double busyPercent(const CpuTimes &before, const CpuTimes &after);

// Samples the whole machine's load for `duration`, polling `cancelled` every
// 50 ms. nullopt if cancelled or unavailable.
std::optional<double> sampleBackgroundLoadPercent(std::chrono::milliseconds duration,
                                                  const std::function<bool()> &cancelled);

// Linux: /sys/firmware/acpi/platform_profile ("balanced"); Windows: the active
// power scheme's name ("Balanced"). Empty if unknown.
std::string powerProfile();
std::string linuxPlatformProfile(const std::string &sysfsRoot);

// Memory the OS says is available now (Linux MemAvailable, Windows
// ullAvailPhys), in bytes. nullopt if unknown.
std::optional<std::uint64_t> availableMemoryBytes();

} // namespace sb::cpu
