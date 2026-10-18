// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The 1 Hz telemetry sampler (Phase 2 design §8.1): a thread that runs
// through the whole CPU test and reads the clock of the CPUs running it (the
// scope the test sets: the pinned core during the single-thread mark, all
// CPUs after), the temperature and the performance limit. Times are seconds
// since start().
//
// A reading is usually microseconds, but an ACPI or embedded-controller
// sensor can take hundreds of milliseconds, so Stop must never wait for one:
// stopWithin() gives the thread a short time to finish and otherwise leaves
// it to end on its own (it owns its Source and its samples through a shared
// state, and calls nothing of the caller's once stopped); joinAbandoned()
// joins such threads later (the next run), and at exit they are detached.

#include "tests/cpu/telemetry/telemetry.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace sb::cpu::telemetry {

struct Sample {
    double t = 0;  // seconds since start()
    std::optional<double> clockMHz;
    std::optional<double> temperatureC;
    std::optional<double> performanceLimitPercent;
};

class Sampler
{
public:
    explicit Sampler(std::shared_ptr<Source> source, std::chrono::milliseconds period = std::chrono::milliseconds(1000));
    ~Sampler();  // stop()
    Sampler(const Sampler &) = delete;
    Sampler &operator=(const Sampler &) = delete;

    // Called on the sampler's thread after each sample, never after stop()
    // or stopWithin() has returned (set before start()).
    void setOnSample(std::function<void(const Sample &)> onSample);
    // Starts the thread; the first sample is taken at once.
    void start();
    // Stops and joins (waits for a reading in progress).
    void stop();
    // Stops, waiting at most `wait` for a reading in progress; true if the
    // thread ended (and was joined), false if it was left to end by itself.
    bool stopWithin(std::chrono::milliseconds wait);
    // Joins the threads stopWithin() left behind (each ends after its reading);
    // any still left at exit are detached, never joined.
    static void joinAbandoned();

    // Which CPUs' clock the following samples read (thread-safe).
    void setScope(const ClockScope &scope);
    // Seconds since start() (0 before it).
    double now() const;
    // The samples so far (thread-safe copy).
    std::vector<Sample> samples() const;

private:
    struct State;
    std::shared_ptr<State> m_state;
    std::thread m_thread;
};

// The median clock of the samples with t0 <= t <= t1 that have one; nullopt
// if none do.
std::optional<double> medianClockMHz(const std::vector<Sample> &samples, double t0, double t1);
// The highest temperature in [t0, t1]; nullopt if none.
std::optional<double> peakTemperatureC(const std::vector<Sample> &samples, double t0, double t1);

} // namespace sb::cpu::telemetry
