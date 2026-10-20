// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pointer chase (Phase 3 design §5.1, §4.1, P3): `p = *(void **)p`, 16
// hops per loop iteration, over a cycle built by buildChaseCycle(). One
// dependent load at a time, so a hop takes the load-to-use latency of
// wherever the line is (L1 ... DRAM).
//
// A unit is a fixed K hops (hopsPerUnit: about 2 ms at the latency
// estimate), so workPerUnit() = K is constant within a measurement, and the
// engine's rate is hops per second: latency = 1e9 / rate (latencyNs). The
// median commutes with that monotone map, so the median latency is
// 1e9 / the median rate.
//
// The rate of a window leaves out the time the chase didn't have its core
// (KernelThread::windowRate). A pinned chase shares its core with
// whatever the OS also runs there until the load balancer moves it away:
// on Tiamat a background process's thread (about a third of CPU 13) stayed
// on the chase core for the first second of the grid, and 4-12 KiB read
// 0.915 ns instead of 0.70.
//   Linux (POSIX)  hops over the thread's own CPU time
//                  (CLOCK_THREAD_CPUTIME_ID) across the window's units:
//                  whatever the scheduler's slice length, only the time
//                  the chase ran counts
//   Windows        (thread CPU time there is 15.6 ms ticks) hops over the
//                  time of the units that took at most 2x the window's
//                  median unit: a quantum lost is tens of ms, far over 2x
//                  a 2 ms unit, so a preempted unit is dropped
// Both stay a mean over the units (not a median: under load the units'
// times are skewed by queueing, and loaded latency is an average, as
// MLC's is). Hops per unit are constant.
//
// Correctness (§5.1, §9): every unit's end pointer must lie inside the
// buffer, on a line boundary; otherwise the unit throws and the measurement
// is an Error ("the pointer chase left its buffer..."). The end pointer
// goes into the unit's checksum, and so into the engine's sink.

#include "tests/cpu/engine/kernel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sb::ram {

// Hops per loop iteration; K is always a multiple of it.
inline constexpr std::uint64_t kHopsPerIteration = 16;
// K's bounds (§5.1): 16 Ki hops is at most 16 ms at 1 us per hop, 64 Mi at
// most 29 ms at the 0.43 ns plausibility floor, so a unit stays well within
// the engine's 100 ms either way.
inline constexpr std::uint64_t kMinHopsPerUnit = 16 * 1024;
inline constexpr std::uint64_t kMaxHopsPerUnit = 64ull * 1024 * 1024;
// The unit length K aims for.
inline constexpr double kUnitTargetNs = 2e6;

// Follows the chain from `start` for `hops` hops (a multiple of 16) and
// returns where it ended. Its own TU, noinline, no IPO.
const void *chaseHops(const void *start, std::uint64_t hops);

// K for a measurement whose latency is expected to be about `estimateNs`
// (the previous grid point's; a target's nearest grid point's): 2 ms worth,
// rounded to a multiple of 16, within [16 Ki, 64 Mi]. A non-positive or
// non-finite estimate (none known) gives the smallest K, 16 Ki: whatever
// the real latency, a unit then stays within Stop's 100 ms (AP-5 M3).
std::uint64_t hopsPerUnit(double estimateNs);

// Whether `p` is a line start inside [base, base + bytes).
bool chaseInRange(const void *p, const std::byte *base, std::size_t bytes, std::size_t lineSize);

// The calling thread's CPU time in seconds (CLOCK_THREAD_CPUTIME_ID);
// nullopt on Windows (the window then uses preemptionFreeRate).
std::optional<double> threadCpuSeconds();

// A unit over this x the window's median unit time counts as preempted.
inline constexpr double kPreemptedUnit = 2.0;
// Hops per second over a window's units (seconds each) without the
// preempted ones; nullopt for no units.
std::optional<double> preemptionFreeRate(const std::vector<double> &unitSeconds, std::uint64_t hopsPerUnit);

// ns per hop from the engine's hops per second.
double latencyNs(double hopsPerSecond);

// P15: a hop faster than 3 cycles at 7 GHz is implausible. The engine's
// ceiling is maxWorkPerCyclePerThread x 7 GHz x 1.5 (P14), so the per-cycle
// figure is 1 / (3 x 1.5).
inline constexpr double kChaseMaxHopsPerCycle = 1.0 / (3.0 * 1.5);

class ChaseKernel : public cpu::Kernel
{
public:
    // The cycle in [base, base + bytes) (built, lines of lineSize); every
    // thread starts at `base`, which is on the cycle (it visits every line).
    ChaseKernel(const std::byte *base, std::size_t bytes, std::size_t lineSize, std::uint64_t hopsPerUnit);
    std::string name() const override { return "Pointer chase"; }
    double workPerUnit() const override { return double(m_hops); }
    double maxWorkPerCyclePerThread() const override { return kChaseMaxHopsPerCycle; }
    std::unique_ptr<cpu::KernelThread> makeThread(int thread) const override;

    std::uint64_t hops() const { return m_hops; }

private:
    const std::byte *m_base;
    std::size_t m_bytes;
    std::size_t m_lineSize;
    std::uint64_t m_hops;
};

} // namespace sb::ram
