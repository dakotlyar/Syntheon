// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Warming the chase core before a latency measurement (checkpoint 3, Zeb's
// Defect A). A core that has been idle doesn't reach its top clock at once:
// on Tiamat (amd-pstate-epp, powersave, EPP balance_performance) a chase
// started on an idle core runs at ~4.38 GHz for about 1.1 s, then the clock
// climbs to 5.7 GHz within ~200 ms, so the first grid points read 0.915 ns
// instead of 0.70. The rate is steady on that 4.38 GHz plateau, so "two
// windows alike" alone would stop on it; where the OS tells the core's clock
// and its top clock, the core is warm once the clock is at 85% of the top
// or more and the last two windows agree; where it doesn't, once a full
// second of windows agree. The caller caps it (2 s) and logs a cap. Pure.

#include <optional>
#include <vector>

namespace sb::ram {

inline constexpr double kWarmClockShare = 0.85;
inline constexpr double kWarmSteady = 0.01;  // windows agree within 1%

// `rates`: the warm-up windows' rates so far, in order. `clockMHz` /
// `topMHz`: the core's clock now and its top clock, when known.
// `steadyWithoutClock`: how many windows must agree when they aren't.
bool chaseCoreWarm(const std::vector<double> &rates, std::optional<double> clockMHz, std::optional<double> topMHz,
                   int steadyWithoutClock);

} // namespace sb::ram
