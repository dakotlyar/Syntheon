// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The loaded-latency load's injection delay (Phase 3 design §5.4, P12): a
// spin of n iterations of an opaque() counter, with no pause / yield (their
// cost varies a lot between CPUs). The load generator times it once
// (calibration) and converts each level's delay, given in ns per 4 KiB
// block, into iterations, so a level means the same on every CPU.

#include <cstdint>

namespace sb::ram {

// Runs `iterations` dependent, opaque increments; returns the count (so the
// loop can't be dropped). Its own TU, noinline, no IPO.
std::uint64_t spinIterations(std::uint64_t iterations);

} // namespace sb::ram
