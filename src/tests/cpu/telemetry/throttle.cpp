// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/telemetry/throttle.h"

#include <cmath>
#include <cstdio>

namespace sb::cpu::telemetry {

namespace {

std::string fixed(double v, int decimals)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

// "−16%" with a real minus sign.
std::string minusPercent(double percent)
{
    return "−" + fixed(std::round(percent), 0) + "%";
}

} // namespace

ThrottleVerdict detectThrottle(const std::vector<WindowTrace> &windows, std::optional<std::uint64_t> countBefore,
                               std::optional<std::uint64_t> countAfter)
{
    ThrottleVerdict v;
    if (countBefore && countAfter && *countAfter > *countBefore) {
        v.detected = true;
        v.counters = true;
        v.counterRise = *countAfter - *countBefore;
    }
    // The window with the largest clock drop under the threshold; failing
    // that, the largest rate drop under its threshold.
    const WindowTrace *clockWindow = nullptr, *rateWindow = nullptr;
    double clockRatio = 1, rateRatio = 1;
    for (const WindowTrace &w : windows) {
        if (!w.multiThread || w.repRates.size() < 2)
            continue;
        const std::size_t last = w.repRates.size() - 1;
        if (w.repClockMHz.size() == w.repRates.size() && w.repClockMHz[0] && w.repClockMHz[last]
            && *w.repClockMHz[0] > 0) {
            const double r = *w.repClockMHz[last] / *w.repClockMHz[0];
            if (r < kClockDropThreshold && r < clockRatio) {
                clockRatio = r;
                clockWindow = &w;
            }
        }
        if (w.repRates[0] > 0) {
            const double r = w.repRates[last] / w.repRates[0];
            if (r < kRateDropThreshold && r < rateRatio) {
                rateRatio = r;
                rateWindow = &w;
            }
        }
    }
    if (clockWindow) {
        v.detected = true;
        v.window = clockWindow->name;
        v.fromMHz = clockWindow->repClockMHz.front();
        v.toMHz = clockWindow->repClockMHz.back();
        v.dropPercent = (1 - clockRatio) * 100;
        v.temperatureC = clockWindow->peakTemperatureC;
    } else if (rateWindow) {
        v.detected = true;
        v.window = rateWindow->name;
        v.byRate = true;
        v.dropPercent = (1 - rateRatio) * 100;
        // The clock, where known, even though it didn't fall past the threshold.
        const std::size_t last = rateWindow->repRates.size() - 1;
        if (rateWindow->repClockMHz.size() == rateWindow->repRates.size()) {
            v.fromMHz = rateWindow->repClockMHz.front();
            v.toMHz = rateWindow->repClockMHz[last];
        }
        v.temperatureC = rateWindow->peakTemperatureC;
    }
    return v;
}

std::string throttleDetail(const ThrottleVerdict &v)
{
    if (!v.detected)
        return {};
    if (v.window.empty())
        return "Intel throttle counters rose by " + std::to_string(v.counterRise);
    if (!v.byRate)
        return v.window + ": clock " + fixed(*v.fromMHz / 1000, 1) + " → " + fixed(*v.toMHz / 1000, 1) + " GHz ("
               + minusPercent(v.dropPercent) + ")";
    return v.window + ": rate " + minusPercent(v.dropPercent) + " from the first repetition to the last";
}

std::string throttleNote(const ThrottleVerdict &v)
{
    if (!v.detected)
        return {};
    const std::string at = v.temperatureC ? " at " + fixed(std::round(*v.temperatureC), 0) + " °C" : std::string();
    if (v.window.empty())
        return "Throttling detected: the CPU's throttle counters rose by " + std::to_string(v.counterRise)
               + " during the test. The score is not adjusted.";
    if (!v.byRate)
        return "Throttling detected in " + v.window + ": clock speed fell from " + fixed(*v.fromMHz / 1000, 1) + " to "
               + fixed(*v.toMHz / 1000, 1) + " GHz (" + minusPercent(v.dropPercent) + ")" + at
               + ". The score is not adjusted.";
    return "Throttling detected in " + v.window + ": the rate fell " + fixed(std::round(v.dropPercent), 0)
           + "% from the first repetition to the last" + at + ". The score is not adjusted.";
}

} // namespace sb::cpu::telemetry
