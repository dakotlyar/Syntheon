// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/sweep/targets.h"

#include <algorithm>
#include <cmath>

namespace sb::ram {

namespace {

constexpr std::uint64_t kRound = 4096;

std::uint64_t roundDown(double bytes)
{
    if (!(bytes > 0))
        return 0;
    const auto b = std::uint64_t(bytes);
    return b - b % kRound;
}

std::string sizeText(std::uint64_t bytes)
{
    constexpr std::uint64_t KiB = 1024, MiB = KiB * 1024;
    if (bytes >= MiB && bytes % MiB == 0)
        return std::to_string(bytes / MiB) + " MiB";
    return std::to_string(bytes / KiB) + " KiB";
}

std::string levelName(int level)
{
    return "L" + std::to_string(level);
}

} // namespace

std::uint64_t dramTarget(std::uint64_t lastLevelBytes)
{
    std::uint64_t want = kDramTargetMin;
    if (lastLevelBytes > 0 && lastLevelBytes <= kDramTargetMax) {
        const std::uint64_t sixteen = lastLevelBytes * 16;
        std::uint64_t p = 1;
        while (p < sixteen)
            p *= 2;
        want = p;
    } else if (lastLevelBytes > kDramTargetMax) {
        want = kDramTargetMax;
    }
    return std::clamp(want, kDramTargetMin, kDramTargetMax);
}

LatencyTargets targetsFromSizes(const std::vector<std::uint64_t> &sizes, std::uint64_t dramBytes, bool fromCurve)
{
    LatencyTargets t;
    t.fromCurve = fromCurve;
    t.dramBytes = dramBytes;
    const auto size = [&](int level) -> std::uint64_t {
        return level >= 1 && std::size_t(level) <= sizes.size() ? sizes[std::size_t(level - 1)] : 0;
    };
    for (int k = 1; k <= 3; ++k) {
        LevelTarget &l = t.caches[k - 1];
        l.level = k;
        l.cacheBytes = size(k);
        if (l.cacheBytes == 0) {
            l.absentReason = fromCurve ? "no " + levelName(k) + " found on the curve" : "no " + levelName(k) + " on this core";
            continue;
        }
        if (k == 1) {
            l.bytes = roundDown(double(l.cacheBytes) / 2);
            if (l.bytes == 0)
                l.absentReason = "the L1 is smaller than 8 KiB";
            continue;
        }
        const std::uint64_t below = size(k - 1);
        if (below == 0) {
            l.absentReason = "the " + levelName(k - 1) + " size is unknown";
            continue;
        }
        const std::uint64_t at = roundDown(std::sqrt(double(below) * double(l.cacheBytes)));
        if (double(at) < 1.5 * double(below) || double(at) > 0.75 * double(l.cacheBytes)) {
            l.absentReason = levelName(k) + " " + sizeText(l.cacheBytes) + " is too close in size to the "
                             + levelName(k - 1) + " (" + sizeText(below) + ")";
            continue;
        }
        l.bytes = at;
    }
    return t;
}

LatencyTargets targetsFromTopology(const isa::CacheTopology &topology)
{
    if (topology.empty()) {
        LatencyTargets t = targetsFromSizes({}, dramTarget(0), false);
        for (LevelTarget &l : t.caches)
            l.absentReason = "the OS reports no caches";
        return t;
    }
    std::vector<std::uint64_t> sizes;
    for (int k = 1; k <= 3; ++k) {
        const isa::CacheLevel *c = topology.level(k);
        sizes.push_back(c ? c->sizeBytes : 0);
    }
    const isa::CacheLevel *last = topology.lastLevel();
    return targetsFromSizes(sizes, dramTarget(last ? last->sizeBytes : 0), false);
}

} // namespace sb::ram
