// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The throttling flag (Phase 2 design §8.2): an engineering rule that never
// changes the score. "Detected" if any of:
//   1. the Intel throttle counters rose during the test;
//   2. within one multi-thread window, repetition 5's median clock is under
//      90% of repetition 1's;
//   3. within one multi-thread window, repetition 5's rate is under 93% of
//      repetition 1's.
// Only ever within a window: different workloads legitimately run at
// different clocks (AVX-512). Rule 3 needs no clock source, so the flag is
// never "unknown"; without clocks it just can't say why. The thresholds are
// starting values, to be checked against the validation data (task 19).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sb::cpu::telemetry {

inline constexpr double kClockDropThreshold = 0.90;
inline constexpr double kRateDropThreshold = 0.93;

// One measurement window's repetitions, as the throttle rule sees them.
struct WindowTrace {
    std::string name;          // "xz compression", for the detail and the note
    bool multiThread = true;   // rules 2 and 3 look at multi-thread windows only
    std::vector<double> repRates;                    // repetitions 1..N, in order
    std::vector<std::optional<double>> repClockMHz;  // each repetition's median clock (nullopt: unknown)
    std::optional<double> peakTemperatureC;          // during the window
};

struct ThrottleVerdict {
    bool detected = false;
    // The window it was seen in (the largest clock drop, else the largest
    // rate drop); empty when only the counters rose.
    std::string window;
    std::optional<double> fromMHz, toMHz;  // repetition 1 and N's median clock, when the clock fell
    double dropPercent = 0;                // the clock's drop, else the rate's (positive)
    bool byRate = false;                   // only the rate fell (no clock drop known)
    std::optional<double> temperatureC;    // the window's peak
    bool counters = false;                 // the Intel counters rose
    std::uint64_t counterRise = 0;
};

ThrottleVerdict detectThrottle(const std::vector<WindowTrace> &windows, std::optional<std::uint64_t> countBefore,
                               std::optional<std::uint64_t> countAfter);

// `throttle.detail`: "xz compression: clock 4.9 → 4.1 GHz (−16%)", "xz
// compression: rate −9% from the first repetition to the last", "Intel
// throttle counters rose by 12"; "" when not detected.
std::string throttleDetail(const ThrottleVerdict &v);
// The Detail page's note (wording approved by Dave 2026-10-16): "Throttling
// detected in xz compression: clock speed fell from 4.6 to 3.9 GHz (−15%) at
// 95 °C. The score is not adjusted." (no " at …" without a temperature).
std::string throttleNote(const ThrottleVerdict &v);

} // namespace sb::cpu::telemetry
