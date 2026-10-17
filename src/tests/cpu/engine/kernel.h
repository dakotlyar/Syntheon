// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the measurement engine runs (Phase 2 design §4, §5, §6): a kernel is
// a fixed amount of work per "unit", done again and again on every worker
// thread until the window's deadline. Each unit returns a checksum, which is
// compared with the kernel's golden value for that seed.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sb::cpu {

// Inputs depend on the seed s = unitIndex mod kSeeds (§5), so a golden table
// has kSeeds entries.
inline constexpr std::uint64_t kSeeds = 64;

// One worker thread's state for one kernel: its buffers and contexts. Made on
// the worker thread itself, so the memory is first touched (and placed) there,
// before any timing (§4.1).
class KernelThread
{
public:
    virtual ~KernelThread() = default;
    // One unit of work for `unitIndex`; returns its checksum. A unit takes at
    // most 100 ms on the slowest supported CPU (§4.1: Stop returns in 250 ms).
    virtual std::uint64_t unit(std::uint64_t unitIndex) = 0;
};

class Kernel
{
public:
    virtual ~Kernel() = default;
    // For messages: "Integer", "xz compression"...
    virtual std::string name() const = 0;
    // The work in one unit, in the unit of the reported rate before scaling
    // (operations, FLOPs, bytes...). A constant: a wrong count is a silent
    // score error (§14.2 item 3).
    virtual double workPerUnit() const = 0;
    // P14: the most work per CPU cycle one thread can plausibly do. A rate
    // above ceiling x 7 GHz x threads x 1.5 means the kernel was optimised
    // away and is an Error.
    virtual double maxWorkPerCyclePerThread() const = 0;
    // Called on the worker thread that will use it.
    virtual std::unique_ptr<KernelThread> makeThread(int thread) const = 0;
    // The golden checksum for a seed (§6.1, §6.4); nothing = no check.
    virtual std::optional<std::uint64_t> golden(std::uint64_t seed) const
    {
        (void)seed;
        return std::nullopt;
    }
};

} // namespace sb::cpu
