// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/sweep/knees.h"

namespace sb::ram {

namespace {

bool steep(const std::vector<CurvePoint> &c, std::size_t i)
{
    return i + 1 < c.size() && c[i].ns > 0 && c[i + 1].ns >= kKneeRise * c[i].ns;
}

} // namespace

std::vector<std::uint64_t> detectKnees(const std::vector<CurvePoint> &curve)
{
    std::vector<std::uint64_t> knees;
    std::size_t i = 0;
    while (i + 2 < curve.size()) {
        if (steep(curve, i) && curve[i + 2].ns > curve[i + 1].ns) {
            knees.push_back(curve[i].bytes);
            // Past the steep part of this transition.
            ++i;
            while (steep(curve, i))
                ++i;
            continue;
        }
        ++i;
    }
    return knees;
}

LatencyTargets targetsFromCurve(const std::vector<CurvePoint> &curve, std::uint64_t dramBytes)
{
    std::vector<std::uint64_t> sizes = detectKnees(curve);
    if (sizes.size() > 3)
        sizes.resize(3);
    return targetsFromSizes(sizes, dramBytes, true);
}

} // namespace sb::ram
