// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Loaded latency (Phase 3 design §5.4, D7, P12): the DRAM chase on the
// chosen core while the LoadGenerator streams reads on every other core,
// at five load levels (delays of 0, 250, 750, 2,000 and 6,000 ns per 4 KiB
// block). Per level: the load runs from before the chase's warm-up until its
// last repetition ends, then parks until the next level.
//
// Load bandwidth per repetition: the coordinator reads the load's byte
// counters at each chase window's start (the engine's progress callback)
// and once more when the measurement returns, so repetition r's load is
// (bytes at r+1's start - bytes at r's start) / the time between. That span
// is the window plus the engine's few microseconds of bookkeeping; the chase
// pool must therefore not re-pick its core between windows (no pickFrom:
// the core is picked once per sub-test, §4.3). The level's load is the
// median over its repetitions.
//
// The latency buffer's DRAM cycle is built by the caller (once, §3), and
// the ChaseKernel's K sized from the idle DRAM latency.

#include "tests/cpu/engine/worker_pool.h"
#include "tests/ram/kernels/chase.h"
#include "tests/ram/loaded/load_generator.h"

#include <functional>
#include <string>
#include <vector>

namespace sb::ram {

inline constexpr double kLoadDelaysNs[] = {0, 250, 750, 2000, 6000};

struct LoadedLevel {
    double delayNs = 0;
    double latencyNs = 0;   // 1e9 / the chase's median rate
    double spread = 0;      // the chase's (max - min) / median
    double loadGBs = 0;     // the median of repLoadGBs
    std::vector<double> repLoadGBs;  // the load's GB/s in each repetition
};

struct LoadedResult {
    cpu::Measurement::Outcome outcome = cpu::Measurement::Outcome::Ok;
    std::string error;              // the chase's, when it isn't Ok
    std::vector<LoadedLevel> levels;  // the levels that finished, in order (all five when Ok)
    double nsPerSpin = 0;           // the delay calibration
    bool ok() const { return outcome == cpu::Measurement::Outcome::Ok; }
};

// Runs the five levels (blocking). `chasePool`: one worker pinned to the
// chase core, no pickFrom. `progress(level, window)`: on this thread at
// each chase window's start (window 0 = the level's warm-up). The load is
// parked when this returns, also on a cancel or an error.
// `beforeLevels` (optional) runs once after the spin calibration, just
// before the first level (the RAM test warms the chase core there, AP-5
// M3); false ends the measurement as Cancelled.
LoadedResult measureLoadedLatency(cpu::WorkerPool &chasePool, const ChaseKernel &chase, LoadGenerator &load,
                                  const cpu::WindowSpec &spec, const std::function<bool()> &cancelled,
                                  const std::function<void(int level, int window)> &progress = {},
                                  const std::function<bool()> &beforeLevels = {});

} // namespace sb::ram
