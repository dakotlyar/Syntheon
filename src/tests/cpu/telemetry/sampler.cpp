// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/telemetry/sampler.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace sb::cpu::telemetry {

struct Sampler::State {
    std::shared_ptr<Source> source;
    std::chrono::milliseconds period;
    std::chrono::steady_clock::time_point start{};
    mutable std::mutex mutex;
    std::condition_variable wake;  // stop requested
    std::condition_variable ended; // the thread has finished
    bool stop = false;
    bool done = false;
    ClockScope scope;
    std::vector<Sample> samples;
    // The callback is called under callbackMutex and only while !stop, so
    // once a stop has taken that mutex no call can start or be in progress.
    std::mutex callbackMutex;
    std::function<void(const Sample &)> onSample;

    void loop();
};

namespace {

// Threads stopWithin() left to end by themselves; joined by joinAbandoned(),
// detached at exit.
struct Abandoned {
    std::mutex mutex;
    std::vector<std::thread> threads;
    // At exit, detach instead of joining: a sensor read that never returns
    // (a hung EC) must not hang Syntheon's exit (AP-5 L6). The threads own
    // their state, so detaching them is safe.
    ~Abandoned()
    {
        std::lock_guard lock(mutex);
        for (std::thread &t : threads)
            t.detach();
    }
    void joinAll()
    {
        std::vector<std::thread> t;
        {
            std::lock_guard lock(mutex);
            t.swap(threads);
        }
        for (std::thread &th : t)
            th.join();
    }
};

Abandoned &abandoned()
{
    static Abandoned a;
    return a;
}

} // namespace

void Sampler::State::loop()
{
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        ClockScope sc;
        std::chrono::steady_clock::time_point t0;
        {
            std::lock_guard lock(mutex);
            if (stop)
                break;
            sc = scope;
            t0 = start;
        }
        // The readings, outside the locks (they read files / PDH, and an
        // ACPI sensor can be slow).
        Sample s;
        s.clockMHz = source->clockMHz(sc);
        s.temperatureC = source->temperatureC();
        s.performanceLimitPercent = source->performanceLimitPercent();
        s.t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard cb(callbackMutex);
            {
                std::lock_guard lock(mutex);
                if (stop)
                    break;
                samples.push_back(s);
            }
            if (onSample)
                onSample(s);
        }
        // Fixed-rate ticks from the start, so a slow reading doesn't drift them.
        next += period;
        std::unique_lock lock(mutex);
        if (wake.wait_until(lock, next, [this] { return stop; }))
            break;
    }
    std::lock_guard lock(mutex);
    done = true;
    ended.notify_all();
}

Sampler::Sampler(std::shared_ptr<Source> source, std::chrono::milliseconds period) : m_state(std::make_shared<State>())
{
    m_state->source = std::move(source);
    m_state->period = period;
}

Sampler::~Sampler()
{
    stop();
}

void Sampler::setOnSample(std::function<void(const Sample &)> onSample)
{
    std::lock_guard cb(m_state->callbackMutex);
    m_state->onSample = std::move(onSample);
}

void Sampler::start()
{
    if (m_thread.joinable())
        return;
    {
        std::lock_guard lock(m_state->mutex);
        m_state->stop = false;
        m_state->done = false;
        m_state->start = std::chrono::steady_clock::now();
    }
    m_thread = std::thread([state = m_state] { state->loop(); });
}

void Sampler::stop()
{
    {
        std::lock_guard cb(m_state->callbackMutex);
        std::lock_guard lock(m_state->mutex);
        m_state->stop = true;
    }
    m_state->wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

bool Sampler::stopWithin(std::chrono::milliseconds wait)
{
    {
        std::lock_guard cb(m_state->callbackMutex);
        std::lock_guard lock(m_state->mutex);
        m_state->stop = true;
    }
    m_state->wake.notify_all();
    if (!m_thread.joinable())
        return true;
    bool ended;
    {
        std::unique_lock lock(m_state->mutex);
        ended = m_state->ended.wait_for(lock, wait, [this] { return m_state->done; });
    }
    if (ended) {
        m_thread.join();
        return true;
    }
    std::lock_guard lock(abandoned().mutex);
    abandoned().threads.push_back(std::move(m_thread));
    return false;
}

void Sampler::joinAbandoned()
{
    abandoned().joinAll();
}

void Sampler::setScope(const ClockScope &scope)
{
    std::lock_guard lock(m_state->mutex);
    m_state->scope = scope;
}

double Sampler::now() const
{
    std::lock_guard lock(m_state->mutex);
    if (m_state->start == std::chrono::steady_clock::time_point{})
        return 0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_state->start).count();
}

std::vector<Sample> Sampler::samples() const
{
    std::lock_guard lock(m_state->mutex);
    return m_state->samples;
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
