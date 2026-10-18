// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The 1 Hz telemetry sampler (Phase 2 design §8.1): a thread that runs
// through the whole CPU test and reads the clock of the CPUs running it (the
// scope the test sets: the pinned core during the single-thread mark, all
// CPUs after), the temperature and the performance limit. Each sample costs
// microseconds. Times are seconds since start().

#include "tests/cpu/telemetry/telemetry.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
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
    explicit Sampler(Source &source, std::chrono::milliseconds period = std::chrono::milliseconds(1000));
    ~Sampler();  // stops and joins
    Sampler(const Sampler &) = delete;
    Sampler &operator=(const Sampler &) = delete;

    // Starts the thread; the first sample is taken at once.
    void start();
    void stop();

    // Which CPUs' clock the following samples read (thread-safe).
    void setScope(const ClockScope &scope);
    // Seconds since start() (0 before it).
    double now() const;
    // The samples so far (thread-safe copy).
    std::vector<Sample> samples() const;

private:
    void loop();

    Source &m_source;
    const std::chrono::milliseconds m_period;
    std::chrono::steady_clock::time_point m_start{};
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false;
    ClockScope m_scope;
    std::vector<Sample> m_samples;
    std::thread m_thread;
};

// The median clock of the samples with t0 <= t <= t1 that have one; nullopt
// if none do.
std::optional<double> medianClockMHz(const std::vector<Sample> &samples, double t0, double t1);
// The highest temperature in [t0, t1]; nullopt if none.
std::optional<double> peakTemperatureC(const std::vector<Sample> &samples, double t0, double t1);

} // namespace sb::cpu::telemetry
