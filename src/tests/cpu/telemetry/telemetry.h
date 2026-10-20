// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// CPU telemetry (Phase 2 design §8.1): the clock of the CPUs running the
// test, a temperature, and the OS's throttle indicators, read as the user.
//
//   Linux    clock: cpuN/cpufreq/scaling_cur_freq, averaged over the CPUs
//            running the test; temperature: hwmon by name priority (k10temp
//            Tdie/Tctl/Tccd, coretemp "Package id 0", a cpu*-thermal zone,
//            another *-thermal zone, then acpitz "ACPI zone"); Intel
//            thermal_throttle counters
//   Windows  PDH: % Processor Performance × Processor Frequency / 100 (the
//            _Total instance, or the pinned core's "<group>,<index>"); the
//            thermal zones' High Precision Temperature (labelled as an ACPI
//            zone, which may not be the CPU sensor); % Performance Limit.
//            Not thread-safe: only the sampler's thread reads it
//
// The Linux readers are pure functions over a sysfs root ("/sys" live, a
// fixture tree in the tests). A Source wraps the live readings for the
// sampler; tests and the dev hook give it fake ones. Plain C++20, no Qt.

#include "isa/core_ranking.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sb::cpu::telemetry {

// --- Linux readers (pure, over a sysfs root) ---

// The mean of cpuN/cpufreq/scaling_cur_freq over `cpus` (kHz) in MHz; the
// CPUs without the file are left out. nullopt if none has it.
std::optional<double> linuxClockMHz(const std::string &sysfsRoot, const std::vector<int> &cpus);
// cpuN/cpufreq/cpuinfo_max_freq (kHz) in MHz: the top clock the driver
// reports (amd-pstate, intel_pstate: the boost clock). nullopt if absent.
std::optional<double> linuxTopClockMHz(const std::string &sysfsRoot, int cpu);

// A temperature input and how to label it ("k10temp Tctl").
struct TempSource {
    std::string inputPath;  // a hwmon temp*_input (millidegrees Celsius)
    std::string label;
};
// The CPU temperature sensor under <sysfsRoot>/class/hwmon, by name priority:
// k10temp (Tdie, else Tctl, else the first Tccd: on Zen 1 / Zen+ Tctl is
// Tdie plus an offset), coretemp ("Package id 0", else its first input),
// cpu_thermal or a "cpu…-thermal" zone, another "…-thermal" zone (their
// first input), then acpitz ("ACPI zone"). nullopt if none is there.
std::optional<TempSource> linuxTempSource(const std::string &sysfsRoot);
// A hwmon temp*_input in °C.
std::optional<double> readHwmonCelsius(const std::string &inputPath);

// Every CPU's thermal_throttle/core_throttle_count plus each package's
// package_throttle_count once (by topology/physical_package_id; every CPU
// of a package shows the same count). Intel; world-readable. nullopt if no
// CPU has them.
std::optional<std::uint64_t> linuxThrottleCount(const std::string &sysfsRoot);

// --- Windows helpers (pure) ---

// PDH: "% Processor Performance" × "Processor Frequency" (MHz) / 100.
double pdhClockMHz(double processorPerformancePercent, double processorFrequencyMHz);
// PDH "High Precision Temperature" is in tenths of a kelvin.
double tenthsOfKelvinToCelsius(double tenthsOfKelvin);

// --- Live readings ---

// Which CPUs' clock to read: all the process may use (the multi-thread
// groups), or the single-thread mark's pinned core.
struct ClockScope {
    bool allCpus = true;
    isa::CoreChoice core;  // when !allCpus
};

class Source
{
public:
    virtual ~Source() = default;
    virtual std::optional<double> clockMHz(const ClockScope &scope) = 0;
    virtual std::optional<double> temperatureC() = 0;
    // What the temperature is ("k10temp Tctl"); empty without one.
    virtual std::string temperatureLabel() const = 0;
    // Intel throttle counters (Linux); nullopt where there are none.
    virtual std::optional<std::uint64_t> throttleCount() = 0;
    // Windows "% Performance Limit"; nullopt elsewhere.
    virtual std::optional<double> performanceLimitPercent() = 0;
};

// This machine's source: sysfs on Linux, PDH on Windows, nothing elsewhere
// (every reading nullopt).
std::unique_ptr<Source> makeSystemSource();

} // namespace sb::cpu::telemetry
