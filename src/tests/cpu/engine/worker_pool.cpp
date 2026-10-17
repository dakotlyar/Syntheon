// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/engine/worker_pool.h"

#include "tests/cpu/engine/platform_threads.h"

#include <algorithm>
#include <cmath>
#include <exception>

namespace sb::cpu {

namespace {

std::atomic<int> g_liveThreads{0};

// The coordinator's poll interval for cancel and the deadline (§4.1).
constexpr std::chrono::milliseconds kPoll{50};

// P14: the fastest clock any supported CPU runs at, with headroom.
constexpr double kCeilingHz = 7e9;
constexpr double kCeilingMargin = 1.5;

// Folds unit checksums into a sink (odd, so no information is lost mod 2^64).
constexpr std::uint64_t kSinkMultiplier = 0x9E3779B97F4A7C15ull;

} // namespace

double medianOf(std::vector<double> values)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

double spreadOf(const std::vector<double> &values)
{
    const double m = medianOf(values);
    if (values.empty() || m <= 0)
        return 0;
    const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
    return (*hi - *lo) / m;
}

double plausibilityCeiling(double maxWorkPerCyclePerThread, int threads)
{
    return maxWorkPerCyclePerThread * kCeilingHz * std::max(1, threads) * kCeilingMargin;
}

int WorkerPool::liveThreads()
{
    return g_liveThreads.load();
}

WorkerPool::WorkerPool(Options options) : m_options(options)
{
    const int n = m_options.threads > 0 ? m_options.threads : logicalCpuCount();
    m_pinned = m_options.pinTo.ranked;
    m_workers.resize(std::size_t(n));
    for (int i = 0; i < n; ++i)
        m_workers[std::size_t(i)].thread = std::thread([this, i] { workerMain(i); });
    // An empty job: once every worker has answered, each has done its pinning,
    // so pinned() is settled from here on.
    bool unused = false;
    runJob(Job::None, {}, {}, unused);
}

WorkerPool::~WorkerPool()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        m_job = Job::Quit;
        ++m_generation;
    }
    m_wake.notify_all();
    for (Worker &w : m_workers) {
        if (w.thread.joinable())
            w.thread.join();
    }
}

void WorkerPool::workerMain(int index)
{
    ++g_liveThreads;
    const int count = int(m_workers.size());
    if (m_options.pinTo.ranked) {
        if (!pinCurrentThread(m_options.pinTo)) {
            m_pinned = false;
            ++m_placementFailures;
        }
    } else if (!spreadOverProcessorGroups(index, count)) {
        ++m_placementFailures;
    }
    optOutOfPowerThrottlingForThisThread();

    Worker &w = m_workers[std::size_t(index)];
    std::uint64_t seen = 0;
    for (;;) {
        Job job;
        Clock::time_point start;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_generation != seen; });
            seen = m_generation;
            job = m_job;
            start = m_windowStart;
        }
        if (job == Job::Quit)
            break;
        try {
            switch (job) {
            case Job::Prepare:
                w.failure.reset();
                w.wrongResult = false;
                w.sink = 0;
                w.nextUnit = 0;
                w.state = m_kernel->makeThread(index);
                break;
            case Job::Release:
                w.state.reset();
                break;
            case Job::Window: {
                w.units = 0;
                w.seconds = 0;
                Clock::time_point last = start;
                // Stop is checked between units only: a unit in progress is
                // finished and counted (so every thread finishes at least one).
                do {
                    // Thread t runs units t, t+1, t+2...: every thread goes
                    // through all kSeeds seeds, and at any moment the threads
                    // work on different ones (an interleaved t, t+N, ... would
                    // give each thread only the seeds = t mod gcd(N, 64): at 64
                    // threads, one seed each).
                    const std::uint64_t unitIndex = w.nextUnit++ + std::uint64_t(index);
                    const std::uint64_t sum = w.state->unit(unitIndex);
                    last = Clock::now();
                    ++w.units;
                    // Multiply-add, not XOR: a kernel's checksums repeat every
                    // 64 seeds, and XOR would cancel them out to 0.
                    w.sink = w.sink * kSinkMultiplier + sum;
                    const std::uint64_t seed = unitIndex % kSeeds;
                    if (const auto expected = m_kernel->golden(seed); expected && *expected != sum) {
                        w.failure = m_kernel->name() + " kernel produced a wrong result (seed " + std::to_string(seed)
                                    + "): possible miscompile or hardware fault";
                        w.wrongResult = true;
                        m_stop = true;
                        break;
                    }
                } while (!m_stop.load(std::memory_order_relaxed));
                w.seconds = std::chrono::duration<double>(last - start).count();
                break;
            }
            case Job::None:
            case Job::Quit:
                break;
            }
        } catch (const std::exception &e) {
            w.failure = m_kernel->name() + ": " + e.what();
            m_stop = true;
        } catch (...) {
            w.failure = m_kernel->name() + ": failed";
            m_stop = true;
        }
        {
            std::lock_guard lock(m_mutex);
            ++m_finished;
        }
        m_done.notify_one();
    }
    --g_liveThreads;
}

void WorkerPool::runJob(Job job, std::chrono::milliseconds length, const std::function<bool()> &cancelled,
                        bool &wasCancelled)
{
    const int n = int(m_workers.size());
    Clock::time_point deadline;
    {
        std::lock_guard lock(m_mutex);
        m_stop = false;
        m_finished = 0;
        m_job = job;
        m_windowStart = Clock::now();
        deadline = m_windowStart + length;
        ++m_generation;
    }
    m_wake.notify_all();

    std::unique_lock lock(m_mutex);
    if (job == Job::Window) {
        // Until the deadline or a cancel; a worker that fails sets stop itself.
        while (m_finished < n && !m_stop.load()) {
            const auto now = Clock::now();
            if (now >= deadline)
                break;
            m_done.wait_for(lock, std::min<Clock::duration>(kPoll, deadline - now));
            if (cancelled && cancelled()) {
                wasCancelled = true;
                break;
            }
        }
        m_stop = true;
    }
    m_done.wait(lock, [&] { return m_finished == n; });
}

Measurement WorkerPool::measure(const Kernel &kernel, const WindowSpec &spec, const std::function<bool()> &cancelled,
                                const std::function<void(int window)> &progress)
{
    Measurement m;
    m_kernel = &kernel;
    bool wasCancelled = false;

    const auto failed = [&] {
        for (const Worker &w : m_workers) {
            if (w.failure) {
                m.outcome = w.wrongResult ? Measurement::Outcome::WrongResult : Measurement::Outcome::Failed;
                m.error = *w.failure;
                return true;
            }
        }
        return false;
    };
    const auto finish = [&] {
        bool unused = false;
        runJob(Job::Release, {}, {}, unused);
        for (const Worker &w : m_workers)
            m.checksum = m.checksum * kSinkMultiplier + w.sink;
        m_kernel = nullptr;
        return m;
    };

    runJob(Job::Prepare, {}, {}, wasCancelled);
    if (failed())
        return finish();
    if (progress)
        progress(0);
    runJob(Job::Window, spec.warmup, cancelled, wasCancelled);
    if (failed())
        return finish();
    if (wasCancelled) {
        m.outcome = Measurement::Outcome::Cancelled;
        return finish();
    }

    std::vector<double> rates;
    for (int r = 1; r <= spec.reps; ++r) {
        if (progress)
            progress(r);
        runJob(Job::Window, spec.rep, cancelled, wasCancelled);
        if (failed())
            return finish();
        if (wasCancelled) {
            m.outcome = Measurement::Outcome::Cancelled;
            return finish();
        }
        RepSample s;
        for (const Worker &w : m_workers) {
            s.units += w.units;
            s.seconds = std::max(s.seconds, w.seconds);
            if (w.seconds > 0)
                s.rate += double(w.units) * kernel.workPerUnit() / w.seconds;
        }
        m.reps.push_back(s);
        rates.push_back(s.rate);
    }
    m.median = medianOf(rates);
    m.spread = spreadOf(rates);

    const double ceiling = plausibilityCeiling(kernel.maxWorkPerCyclePerThread(), threads());
    if (!(m.median <= ceiling)) {  // also catches NaN
        m.outcome = Measurement::Outcome::Implausible;
        m.error = kernel.name() + " is implausibly fast (" + std::to_string(m.median) + " per second; the most "
                  + std::to_string(threads()) + " threads can do is " + std::to_string(ceiling)
                  + "): the work may have been optimised away";
    }
    return finish();
}

} // namespace sb::cpu
