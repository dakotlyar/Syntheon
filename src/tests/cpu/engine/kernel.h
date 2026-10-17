// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the measurement engine runs (Phase 2 design §4, §5, §6): a kernel is
// a fixed amount of work per "unit", done again and again on every worker
// thread until the window's deadline. Each unit returns a checksum, which is
// compared with the kernel's golden value for that seed.

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sb::cpu {

// Inputs depend on the seed s = unitIndex mod kSeeds (§5), so a golden table
// has kSeeds entries. Each worker runs consecutive unit indices (offset by its
// thread number), so every thread goes through every seed.
inline constexpr std::uint64_t kSeeds = 64;

// One worker thread's state for one kernel: its buffers and contexts. Made on
// the worker thread itself, so the memory is first touched (and placed) there,
// before any timing (§4.1). Destroyed on the coordinator, once the worker has
// finished with it: the destructor mustn't depend on the thread it runs on.
class KernelThread
{
public:
    virtual ~KernelThread() = default;
    // One unit of work for `unitIndex`; returns its checksum. A unit takes at
    // most 100 ms on the slowest supported CPU (§4.1: Stop returns in 250 ms),
    // or checks cancelRequested() often enough (the codecs: every 64 KiB).
    virtual std::uint64_t unit(std::uint64_t unitIndex) = 0;

    // A kernel that times parts of its units itself (the single-thread
    // compression mark times each codec call on its own, §5.4) reports its
    // own rate for a window, in work per second; nothing = the engine's
    // W_t / E_t. beginWindow() runs on the worker before the window's first
    // unit, windowRate() after its last.
    virtual void beginWindow() {}
    virtual std::optional<double> windowRate() const { return std::nullopt; }

protected:
    // True once the measurement is cancelled (Stop). A unit that may run
    // longer than the rule above returns early when it sees it; the engine
    // then discards that unit (its result may be partial).
    bool cancelRequested() const { return m_cancel && m_cancel->load(std::memory_order_relaxed); }
    // The flag itself, for code that polls it (the codec adapters).
    const std::atomic<bool> *cancelFlag() const { return m_cancel; }

private:
    friend class WorkerPool;
    const std::atomic<bool> *m_cancel = nullptr;
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
    // Called on the worker thread that will use it. Not interruptible by Stop:
    // keep it quick (large shared inputs are shared read-only, not copied).
    virtual std::unique_ptr<KernelThread> makeThread(int thread) const = 0;
    // 0: every thread runs its own units (the default). N > 0: a collective
    // kernel built for exactly N threads (n-body, §5.7): all N work on the
    // same unit together (the kernel synchronises them inside it), so they
    // run the same unit indices, each is credited workPerUnit() / N, and the
    // engine takes the stop decision once for all of them after each unit.
    // A pool with another thread count fails the measurement. A collective
    // unit must not throw (the other threads would wait for it forever).
    virtual int collectiveThreads() const { return 0; }
    // The number of distinct inputs: unit i works on input i mod seeds()
    // (kSeeds; the compression kernels: one per corpus block, 128).
    virtual std::uint64_t seeds() const { return kSeeds; }
    // The golden checksum for a seed (< seeds(); §6.1, §6.4); nothing = no check.
    virtual std::optional<std::uint64_t> golden(std::uint64_t seed) const
    {
        (void)seed;
        return std::nullopt;
    }
};

} // namespace sb::cpu
