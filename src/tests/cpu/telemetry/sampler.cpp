// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/telemetry/sampler.h"

#include <algorithm>

namespace sb::cpu::telemetry {

Sampler::Sampler(Source &source, std::chrono::milliseconds period) : m_source(source), m_period(period) {}

Sampler::~Sampler()
{
    stop();
}

void Sampler::start()
{
    if (m_thread.joinable())
        return;
    {
        std::lock_guard lock(m_mutex);
        m_stop = false;
        m_start = std::chrono::steady_clock::now();
    }
    m_thread = std::thread([this] { loop(); });
}

void Sampler::stop()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void Sampler::setScope(const ClockScope &scope)
{
    std::lock_guard lock(m_mutex);
    m_scope = scope;
}

double Sampler::now() const
{
    std::lock_guard lock(m_mutex);
    if (m_start == std::chrono::steady_clock::time_point{})
        return 0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
}

std::vector<Sample> Sampler::samples() const
{
    std::lock_guard lock(m_mutex);
    return m_samples;
}

void Sampler::loop()
{
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        ClockScope scope;
        std::chrono::steady_clock::time_point start;
        {
            std::lock_guard lock(m_mutex);
            if (m_stop)
                return;
            scope = m_scope;
            start = m_start;
        }
        // The readings, outside the lock (they read files / PDH).
        Sample s;
        s.clockMHz = m_source.clockMHz(scope);
        s.temperatureC = m_source.temperatureC();
        s.performanceLimitPercent = m_source.performanceLimitPercent();
        s.t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::unique_lock lock(m_mutex);
        m_samples.push_back(s);
        // Fixed-rate ticks from the start, so a slow reading doesn't drift them.
        next += m_period;
        if (m_wake.wait_until(lock, next, [this] { return m_stop; }))
            return;
    }
}

namespace {

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

} // namespace

std::optional<double> medianClockMHz(const std::vector<Sample> &samples, double t0, double t1)
{
    std::vector<double> v;
    for (const Sample &s : samples) {
        if (s.t >= t0 && s.t <= t1 && s.clockMHz)
            v.push_back(*s.clockMHz);
    }
    if (v.empty())
        return std::nullopt;
    return median(std::move(v));
}

std::optional<double> peakTemperatureC(const std::vector<Sample> &samples, double t0, double t1)
{
    std::optional<double> peak;
    for (const Sample &s : samples) {
        if (s.t >= t0 && s.t <= t1 && s.temperatureC && (!peak || *s.temperatureC > *peak))
            peak = s.temperatureC;
    }
    return peak;
}

} // namespace sb::cpu::telemetry
