// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/sweep/warmup.h"

#include <algorithm>
#include <cmath>

namespace sb::ram {

namespace {

// The last `n` rates agree within kWarmSteady of each other.
bool steady(const std::vector<double> &rates, int n)
{
    if (n < 2 || rates.size() < std::size_t(n))
        return false;
    const auto first = rates.end() - n;
    const auto [lo, hi] = std::minmax_element(first, rates.end());
    return *lo > 0 && (*hi - *lo) <= kWarmSteady * *lo;
}

} // namespace

bool chaseCoreWarm(const std::vector<double> &rates, std::optional<double> clockMHz, std::optional<double> topMHz,
                   int steadyWithoutClock)
{
    if (clockMHz && topMHz && *clockMHz > 0 && *topMHz > 0)
        return *clockMHz >= kWarmClockShare * *topMHz && steady(rates, 2);
    return steady(rates, std::max(2, steadyWithoutClock));
}

} // namespace sb::ram
