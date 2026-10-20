// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Warming the chase core before a latency measurement (checkpoint 3, Zeb's
// Defect A). A core that has been idle doesn't reach its top clock at once:
// on Tiamat (amd-pstate-epp, powersave, EPP balance_performance) a chase
// started on an idle core runs at ~4.38 GHz for about 1.1 s, then the clock
// climbs to 5.7 GHz within ~200 ms, so the first grid points read 0.915 ns
// instead of 0.70. The rate is steady on that 4.38 GHz plateau (about the
// part's 4.3 GHz base), so steady windows prove nothing by themselves:
//   - where the OS gives the core's clock and an honest top clock (Linux
//     cpufreq cpuinfo_max_freq), the core is warm once the clock is at 85%
//     of the top or more and the last two windows agree within 1%;
//   - where it doesn't (Windows, whose "max MHz" is the base clock; Linux
//     without cpufreq), the core is never declared warm early: the caller
//     always spins its full cap (2 s; Dave, 2026-10-20).
// Pure.

#include <optional>
#include <vector>

namespace sb::ram {

inline constexpr double kWarmClockShare = 0.85;
inline constexpr double kWarmSteady = 0.01;  // windows agree within 1%

// The last `n` window rates agree within kWarmSteady.
bool warmRatesSteady(const std::vector<double> &rates, int n);

// `rates`: the warm-up windows' rates so far, in order. `clockMHz` /
// `topMHz`: the core's clock now and its top clock, when known; without
// both, false (the caller runs its cap).
bool chaseCoreWarm(const std::vector<double> &rates, std::optional<double> clockMHz, std::optional<double> topMHz);

} // namespace sb::ram
