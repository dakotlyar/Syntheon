// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/loaded/load_generator.h"

#include "tests/cpu/engine/platform_threads.h"
#include "tests/ram/kernels/spin.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace sb::ram {

namespace {

std::atomic<int> g_liveLoadThreads{0};

} // namespace

std::vector<isa::CoreChoice> loadCpus(const std::vector<isa::CoreChoice> &all, const std::vector<int> &chaseSmtKeys)
{
    std::vector<isa::CoreChoice> out;
    for (const isa::CoreChoice &c : all) {
        if (std::find(chaseSmtKeys.begin(), chaseSmtKeys.end(), isa::cpuKey(c)) == chaseSmtKeys.end())
            out.push_back(c);
    }
    return out;
}

LoadGenerator::LoadGenerator(BwPath path, std::vector<LoadThreadSpec> threads)
    : m_read(bandwidthFunctions(path).read), m_specs(std::move(threads)),
      m_counters(std::make_unique<Counter[]>(m_specs.size())), m_startedOn(m_specs.size(), -1)
{
    if (!m_read)
        throw std::invalid_argument("LoadGenerator: this build has no code for the bandwidth path");
    for (const LoadThreadSpec &s : m_specs) {
        if (!s.data || s.bytes == 0 || s.bytes % kLoadBlock != 0)
            throw std::invalid_argument("LoadGenerator: each thread needs a slice of whole 4 KiB blocks");
    }
    m_threads.reserve(m_specs.size());
    for (std::size_t i = 0; i < m_specs.size(); ++i)
        m_threads.emplace_back([this, i] { threadMain(i); });
    // Settle the placement before returning (placementFailures, startedOn).
    std::unique_lock lock(m_mutex);
    m_done.wait(lock, [&] { return m_started == int(m_threads.size()); });
}

LoadGenerator::~LoadGenerator()
{
    {
        std::lock_guard lock(m_mutex);
        m_parkFlag = true;
        m_job = Job::Quit;
        m_acked = 0;
        ++m_generation;
    }
    m_wake.notify_all();
    for (std::thread &t : m_threads) {
        if (t.joinable())
            t.join();
    }
}

int LoadGenerator::liveThreads()
{
    return g_liveLoadThreads.load();
}

void LoadGenerator::threadMain(std::size_t index)
{
    ++g_liveLoadThreads;
    const LoadThreadSpec &spec = m_specs[index];
    if (spec.cpu.ranked && !cpu::pinCurrentThread(spec.cpu))
        ++m_placementFailures;
    cpu::optOutOfPowerThrottlingForThisThread();
    std::uint64_t seen = 0;
    {
        std::lock_guard lock(m_mutex);
        m_startedOn[index] = cpu::currentCpu();
        seen = m_generation;
        ++m_started;
    }
    m_done.notify_all();

    Counter &counter = m_counters[index];
    std::uint64_t sum = 0;
    std::size_t at = 0;
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_generation != seen; });
            seen = m_generation;
            job = m_job;
        }
        if (job == Job::Quit)
            break;
        if (job == Job::Calibrate && index == 0) {
            // A core that just woke may still be ramping its clock: spin
            // for 20 ms first (the ~50 ms of §3's budget). Then best of
            // three: a preemption only makes a timing longer.
            const auto warm = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            while (std::chrono::steady_clock::now() < warm)
                sum += spinIterations(kSpinCalibrationIterations / 10);
            double best = 0;
            for (int i = 0; i < 3; ++i) {
                const auto start = std::chrono::steady_clock::now();
                sum += spinIterations(kSpinCalibrationIterations);
                const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
                best = i == 0 ? ns : std::min(best, ns);
            }
            m_nsPerIteration = best / double(kSpinCalibrationIterations);  // read after the ack, under the mutex
        }
        if (job == Job::Run) {
            // Stream until parked: the flag is polled after every block.
            while (!m_parkFlag.load(std::memory_order_relaxed)) {
                sum += m_read(spec.data + at, kLoadBlock);
                at += kLoadBlock;
                if (at == spec.bytes)
                    at = 0;
                counter.bytes.store(counter.bytes.load(std::memory_order_relaxed) + kLoadBlock,
                                    std::memory_order_relaxed);
                if (const std::uint64_t spins = m_spins.load(std::memory_order_relaxed))
                    sum += spinIterations(spins);
            }
            continue;  // the park that stopped it is the next generation (or already is)
        }
        {
            std::lock_guard lock(m_mutex);
            ++m_acked;
        }
        m_done.notify_all();
    }
    m_sink.fetch_add(sum, std::memory_order_relaxed);
    --g_liveLoadThreads;
}

double LoadGenerator::calibrate()
{
    park();
    if (m_threads.empty())
        return 0;
    std::unique_lock lock(m_mutex);
    m_job = Job::Calibrate;
    m_acked = 0;
    ++m_generation;
    m_wake.notify_all();
    m_done.wait(lock, [&] { return m_acked == int(m_threads.size()); });
    m_job = Job::Park;
    return m_nsPerIteration;
}

void LoadGenerator::run(double delayNs)
{
    const double spins = m_nsPerIteration > 0 && delayNs > 0 ? std::round(delayNs / m_nsPerIteration) : 0.0;
    m_spins.store(std::uint64_t(spins), std::memory_order_relaxed);
    std::lock_guard lock(m_mutex);
    if (m_job == Job::Run)
        return;  // re-levelled in place
    m_parkFlag = false;
    m_job = Job::Run;
    m_acked = 0;
    ++m_generation;
    m_wake.notify_all();
}

void LoadGenerator::park()
{
    std::unique_lock lock(m_mutex);
    if (m_job == Job::Park)
        return;
    m_parkFlag = true;
    m_job = Job::Park;
    m_acked = 0;
    ++m_generation;
    m_wake.notify_all();
    m_done.wait(lock, [&] { return m_acked == int(m_threads.size()); });
}

std::uint64_t LoadGenerator::bytesRead() const
{
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < m_specs.size(); ++i)
        total += m_counters[i].bytes.load(std::memory_order_relaxed);
    return total;
}

std::vector<int> LoadGenerator::startedOn() const
{
    return m_startedOn;
}

} // namespace sb::ram
