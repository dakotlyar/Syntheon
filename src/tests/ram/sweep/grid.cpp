// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/sweep/grid.h"

#include <algorithm>
#include <cmath>

namespace sb::ram {

std::vector<std::uint64_t> latencyGrid(std::uint64_t maxBytes)
{
    std::vector<std::uint64_t> grid;
    for (std::uint64_t p = kGridFirst; p <= maxBytes; p *= 2) {
        grid.push_back(p);
        if (p + p / 2 <= maxBytes)
            grid.push_back(p + p / 2);
        if (p > maxBytes / 2)
            break;
    }
    return grid;
}

std::chrono::milliseconds chaseWarmup(std::uint64_t bytes, std::uint64_t lineSize, double estimateNs,
                                      std::chrono::milliseconds lo, std::chrono::milliseconds hi)
{
    const double lines = lineSize ? double(bytes / lineSize) : 0.0;
    const double lapMs = std::isfinite(estimateNs) && estimateNs > 0 ? lines * estimateNs / 1e6 : 0.0;
    const auto ms = std::chrono::milliseconds(std::int64_t(std::ceil(std::min(lapMs, double(hi.count())))));
    return std::clamp(ms, lo, hi);
}

} // namespace sb::ram
