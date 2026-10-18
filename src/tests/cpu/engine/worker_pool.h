// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The measurement engine (Phase 2 design §4): a pool of worker threads that
// runs a kernel in time-boxed windows.
//
// One measurement = an unscored warm-up, then `reps` repetitions of a fixed
// length (P1). In each window every thread runs units until the coordinator
// sets `stop`; it finishes the unit it is in (units are <= 100 ms) and records
// its work W_t and the time E_t from the window's start to the end of its own
// last unit. A repetition's rate is R = sum over threads of W_t / E_t (P2): no
// thread is charged for another's last, long unit. (A kernel that times the
// parts of its units itself, KernelThread::windowRate, gives its own rate for
// the window instead of W_t / E_t.) The result is the median of the
// repetitions, with spread = (max - min) / median.
//
// The coordinator is the thread that calls measure() (the CPU test's run()
// thread). It polls `cancelled` every 50 ms, and once more at the end of each
// window; the workers never call into the test's context. Stop therefore returns within about 150 ms: a poll, plus
// the unit each worker is in. A unit that can run longer (a codec call on a
// 256 KiB block) polls KernelThread::cancelRequested(), set on a cancel,
// every 64 KiB and returns early; a cancelled unit is neither counted nor
// checked. For that to hold when every CPU is busy, the
// coordinator runs at a raised priority during measure() (Windows; see
// ScopedRaisedThreadPriority), and once the workers have stopped it frees
// their kernel state itself rather than waking each of them again.
//
// Stop is honoured in the windows only: the per-measurement Prepare step
// (each worker's makeThread()) isn't interruptible, so the 250 ms Stop
// contract also needs makeThread() to be quick (no per-thread copies of large
// inputs such as the 32 MiB corpus; share them read-only), and so do the
// KernelThread destructors, which run on the coordinator.
//
// Every unit's checksum goes to the kernel's golden check (a mismatch is an
// Error and ends the measurement) and is folded (multiply-add, which, unlike
// XOR, doesn't cancel when a kernel's checksums repeat every 64 seeds) into a
// per-thread sink that ends in Measurement::checksum, so no result is dead
// code (§6.1).
//
// Threads: one per logical CPU for the multi-thread groups, not pinned (P3;
// on Windows with several processor groups, spread at group level); one
// thread pinned to the top-ranked core for the single-thread mark (P4). When
// several cores tie at the top, the single-thread pool re-picks before the
// warm-up and each repetition (decision D, Options::pickFrom): it samples
// idle time for 250 ms (before the window starts, so the sampling counts
// toward no window; it polls cancel every 50 ms like a window does) and
// re-pins to the tied core whose physical core was idle the most. The threads
// start with the pool and are joined when it is destroyed, before the test's
// run() returns (Phase 1 threading rule 4).

#include "isa/core_ranking.h"
#include "tests/cpu/engine/kernel.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sb::cpu {

using Clock = std::chrono::steady_clock;
static_assert(Clock::is_steady, "the engine times with a monotonic clock (P6)");

struct WindowSpec {
    std::chrono::milliseconds warmup{1000};
    std::chrono::milliseconds rep{3000};
    int reps = 5;
};

struct RepSample {
    double rate = 0;          // work per second, summed over threads
    std::uint64_t units = 0;  // units finished, all threads (a collective kernel's shared unit counts once)
    double seconds = 0;       // the longest thread's E_t
    // When the coordinator set stop (the deadline; for the tests, which check
    // that no thread starts more than the one unit racing with it).
    Clock::time_point stopSet{};
    // The core the workers were pinned to (decision D's pick, else pinTo);
    // unranked: they ran unpinned. For the log and the Detail page.
    isa::CoreChoice core;
    // Decision D's re-pin before this repetition failed: the workers stayed
    // where they were (`core`). For the log.
    bool repickFailed = false;
};

struct Measurement {
    // WrongResult: a golden check failed; Implausible: faster than the P14
    // ceiling; Failed: a kernel threw (e.g. out of memory). All three make the
    // sub-test an Error with `error` as its reason.
    enum class Outcome { Ok, Cancelled, WrongResult, Implausible, Failed };
    Outcome outcome = Outcome::Ok;
    std::string error;            // the reason, for the log and the result
    std::vector<RepSample> reps;  // the repetitions that ran, in order
    double median = 0;            // work per second
    double spread = 0;            // (max - min) / median
    std::uint64_t checksum = 0;   // every unit's checksum folded together, all threads

    bool ok() const { return outcome == Outcome::Ok; }
    // A spread over 10% is logged: "unstable: background activity or throttling" (§4.2).
    bool unstable() const { return spread > 0.10; }
};

// Pure helpers (unit-tested).
double medianOf(std::vector<double> values);
double spreadOf(const std::vector<double> &values);  // (max - min) / median; 0 for none
// P14: the plausibility ceiling, in work per second.
double plausibilityCeiling(double maxWorkPerCyclePerThread, int threads);

class WorkerPool
{
public:
    struct Options {
        int threads = 0;            // 0 = logicalCpuCount()
        isa::CoreChoice pinTo;      // ranked: every worker is pinned there (the single-thread mark)
        // Decision D: with two or more candidates, before the warm-up and
        // each repetition the pool samples idle time for `sampleFor` and
        // re-pins every worker to isa::quietestCandidate(). Empty or one:
        // pinTo throughout. For the single-thread pool.
        std::vector<isa::CoreCandidate> pickFrom;
        std::chrono::milliseconds sampleFor{250};
        std::function<isa::IdleSample()> idleSampler;  // empty: isa::sampleIdle (tests inject their own)
    };
    // The single-thread mark's pool on this machine: one thread on the
    // top-ranked core, re-picked among the tied ones per repetition.
    static Options singleThreadOptions();
    explicit WorkerPool(Options options);
    ~WorkerPool();  // stops and joins every worker
    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    int threads() const { return int(m_workers.size()); }
    // Whether the workers of a pool made with a ranked pinTo really are pinned
    // (false: the call failed and they run unpinned). Settled on construction.
    bool pinned() const { return m_pinned.load(); }

    // Runs one measurement of `kernel` (blocking). `progress` (optional) is
    // called on this thread at each window's start: 0 for the warm-up, then
    // 1..reps.
    Measurement measure(const Kernel &kernel, const WindowSpec &spec, const std::function<bool()> &cancelled,
                        const std::function<void(int window)> &progress = {});

    // Worker threads alive in this process, all pools (tests: a pool's
    // threads are gone once it is destroyed).
    static int liveThreads();
    // The workers' OS thread ids (currentThreadId()), in worker order; for
    // tests (e.g. reading /proc/self/task/<tid>/schedstat).
    std::vector<long> threadIds() const;
    // Workers whose placement failed (pinning, or a processor-group
    // assignment on Windows): they run where the OS puts them. For the log.
    int placementFailures() const { return m_placementFailures.load(); }
    // Where the workers are pinned right now (unranked: unpinned). Read it on
    // the coordinator, e.g. in measure()'s progress callback: at each window's
    // start it is that window's core (decision D picks before the callback).
    isa::CoreChoice currentCore() const { return m_core; }

private:
    enum class Job { None, Prepare, Window, Pin, Quit };
    struct Worker {
        std::thread thread;
        std::unique_ptr<KernelThread> state;
        // Results of the last window (written by the worker, read after it reports done).
        std::uint64_t units = 0;
        double seconds = 0;  // E_t
        std::optional<double> ownRate;  // the kernel's own rate for the window (KernelThread::windowRate)
        std::uint64_t sink = 0;
        std::uint64_t nextUnit = 0;  // this thread's unit counter in the current measurement
        long tid = 0;                // set by the worker before the constructor's first job returns
        bool pinFailed = false;      // the last Pin job's pinCurrentThread() failed
        std::optional<std::string> failure;
        bool wrongResult = false;    // the failure is a golden mismatch
    };

    void workerMain(int index);
    // Decision D, before a window: samples, picks and re-pins (m_core; on a
    // failed re-pin m_core stays and m_repickFailed is set). False if
    // cancelled while sampling.
    bool repick(const std::function<bool()> &cancelled, bool &wasCancelled);
    // Hands `job` to every worker and waits until all have finished it.
    void runJob(Job job, std::chrono::milliseconds length, const std::function<bool()> &cancelled, bool &wasCancelled);

    Options m_options;
    std::vector<Worker> m_workers;
    std::atomic<bool> m_pinned{false};
    std::atomic<int> m_placementFailures{0};
    const Kernel *m_kernel = nullptr;
    isa::CoreChoice m_core;       // where the workers are pinned now (unranked: unpinned)
    bool m_repickFailed = false;  // the last repick()'s re-pin failed
    isa::CoreChoice m_pinTarget;  // for a Pin job (under m_mutex)

    std::mutex m_mutex;
    std::condition_variable m_wake;   // workers wait for a new generation
    std::condition_variable m_done;   // the coordinator waits for `m_finished`
    std::uint64_t m_generation = 0;
    Job m_job = Job::None;
    int m_finished = 0;
    Clock::time_point m_windowStart;
    Clock::time_point m_stopSet;  // when the coordinator last set stop (under m_mutex)
    std::atomic<bool> m_stop{false};
    // Set with m_stop on a cancel, for the units that poll it
    // (KernelThread::cancelRequested); cleared when a measurement starts.
    std::atomic<bool> m_cancel{false};

    // Collective kernels (Kernel::collectiveThreads): after each unit every
    // worker arrives here, and the barrier's completion reads m_stop once for
    // all of them, so they all run the same number of units.
    struct DecideStop {
        WorkerPool *pool;
        void operator()() noexcept { pool->m_collectiveStop = pool->m_stop.load(); }
    };
    std::unique_ptr<std::barrier<DecideStop>> m_unitBarrier;
    bool m_collectiveStop = false;  // written by the completion, read after the barrier
};

} // namespace sb::cpu
