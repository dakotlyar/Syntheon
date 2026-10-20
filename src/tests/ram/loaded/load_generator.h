// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The loaded-latency load (Phase 3 design §5.4, P12, D7, D8): one thread
// pinned to every logical CPU except the chase core and its SMT siblings
// (loadCpus), each streaming reads over its own bandwidth slice with §5.3's
// read loop (D8: read-only traffic), 4 KiB at a time, and spinning for the
// level's delay after every block.
//
// Lifecycle (Phase 1 threading rule 4): the threads start with the
// generator, pinned and parked on a condition variable; run(delay) sets them
// streaming, park() stops them (each finishes the block it is in: a 4 KiB
// read and at most one delay, microseconds) and returns once all are parked;
// the destructor parks and joins them, so they are gone before the test's
// run() returns. A thread checks the park flag after every block, so Stop
// is immediate.
//
// Delay calibration: calibrate() has the first load thread spin for 20 ms
// (so its clock has ramped up), then time 10^6 iterations of the spin loop
// (spin.h) three times, keeping the fastest (a preemption only adds time);
// the levels' delays in ns per 4 KiB become iterations from that.
// Known limit (AP-5 L4): the timing is taken on one core while the others
// are parked, so at a boosted clock; under full load all-core clocks are
// lower, an iteration takes longer, and the real delays come out longer
// than nominal by about the ratio of the two clocks (on Tiamat, a 5.x GHz
// single-core boost against a lower all-core clock). The levels are
// therefore "nominal ns", the same calibration on every run of a machine;
// the curve's x axis is the measured load bandwidth, not the delay, so
// that's what is compared.
//
// Load bandwidth: every thread adds its bytes to its own relaxed counter
// (one per cache line); bytesRead() sums them, so the coordinator can read
// the load's traffic at each chase window's start and end.

#include "isa/core_ranking.h"
#include "tests/ram/kernels/bw_kernels.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sb::ram {

inline constexpr std::size_t kLoadBlock = 4096;
inline constexpr std::uint64_t kSpinCalibrationIterations = 1000000;

// P12: the load threads' CPUs: every CPU in `all` whose key (isa::cpuKey) is
// not in `chaseSmtKeys` (the chase core's physical core: itself and its SMT
// siblings), in `all`'s order.
std::vector<isa::CoreChoice> loadCpus(const std::vector<isa::CoreChoice> &all, const std::vector<int> &chaseSmtKeys);

struct LoadThreadSpec {
    isa::CoreChoice cpu;          // ranked: pinned there; unranked: where the OS puts it
    const std::byte *data = nullptr;  // the slice it streams over (64-byte aligned)
    std::size_t bytes = 0;            // a multiple of 4 KiB
};

class LoadGenerator
{
public:
    // Starts one parked thread per spec, each pinned to its CPU.
    LoadGenerator(BwPath path, std::vector<LoadThreadSpec> threads);
    ~LoadGenerator();  // parks and joins every thread
    LoadGenerator(const LoadGenerator &) = delete;
    LoadGenerator &operator=(const LoadGenerator &) = delete;

    int threads() const { return int(m_threads.size()); }
    // Threads whose pinning failed (they run where the OS puts them); for
    // the log. Settled once the constructor returns.
    int placementFailures() const { return m_placementFailures.load(); }

    // The spin loop's ns per iteration, timed on the first thread (blocking;
    // about 21 ms). 0 if there are no threads.
    double calibrate();
    double nsPerIteration() const { return m_nsPerIteration; }

    // Every thread streams, spinning delayNs (rounded to whole iterations of
    // the last calibration; none before one) after every 4 KiB block.
    // Returns at once; a running load is re-levelled in place.
    void run(double delayNs);
    // Every thread stops after its current block; returns once all are parked.
    void park();

    // Bytes read by all threads since the generator started (relaxed).
    std::uint64_t bytesRead() const;
    // Where each thread ran when it started (currentCpu(); -1 unknown), for tests.
    std::vector<int> startedOn() const;

    // Load threads alive in this process (tests: none after destruction).
    static int liveThreads();

private:
    enum class Job { Park, Run, Calibrate, Quit };
    struct alignas(64) Counter {
        std::atomic<std::uint64_t> bytes{0};
    };
    void threadMain(std::size_t index);
    void stopAndJoin();

    std::uint64_t (*m_read)(const void *, std::size_t);
    std::vector<LoadThreadSpec> m_specs;
    std::unique_ptr<Counter[]> m_counters;
    std::vector<std::thread> m_threads;
    std::vector<int> m_startedOn;
    std::atomic<int> m_placementFailures{0};
    double m_nsPerIteration = 0;

    std::mutex m_mutex;
    std::condition_variable m_wake;  // threads wait for a new generation
    std::condition_variable m_done;  // the coordinator waits for m_acked
    std::uint64_t m_generation = 0;
    Job m_job = Job::Park;
    int m_acked = 0;        // threads that took the current job (Park: that parked)
    int m_started = 0;      // threads that finished their start-up (pinning)
    std::atomic<bool> m_parkFlag{true};  // polled after every block
    std::atomic<std::uint64_t> m_spins{0};  // iterations after every block
    std::atomic<std::uint64_t> m_sink{0};
};

} // namespace sb::ram
