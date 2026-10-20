// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/sweep/warmup.h"

#include <algorithm>

namespace sb::ram {

bool warmRatesSteady(const std::vector<double> &rates, int n)
{
    if (n < 2 || rates.size() < std::size_t(n))
        return false;
    const auto first = rates.end() - n;
    const auto [lo, hi] = std::minmax_element(first, rates.end());
    return *lo > 0 && (*hi - *lo) <= kWarmSteady * *lo;
}

bool chaseCoreWarm(const std::vector<double> &rates, std::optional<double> clockMHz, std::optional<double> topMHz)
{
    if (!clockMHz || !topMHz || *clockMHz <= 0 || *topMHz <= 0)
        return false;
    return *clockMHz >= kWarmClockShare * *topMHz && warmRatesSteady(rates, 2);
}

} // namespace sb::ram
