// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/kernels/chase.h"

#include "tests/cpu/engine/opaque.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <vector>

#if !defined(_WIN32)
#include <time.h>
#endif

namespace sb::ram {

std::uint64_t hopsPerUnit(double estimateNs)
{
    if (!(estimateNs > 0) || !std::isfinite(estimateNs))
        return kMinHopsPerUnit;
    const double raw = kUnitTargetNs / estimateNs;
    const double clamped = std::clamp(raw, double(kMinHopsPerUnit), double(kMaxHopsPerUnit));
    const auto k = std::uint64_t(clamped);
    return std::clamp(k - k % kHopsPerIteration, kMinHopsPerUnit, kMaxHopsPerUnit);
}

bool chaseInRange(const void *p, const std::byte *base, std::size_t bytes, std::size_t lineSize)
{
    const auto at = reinterpret_cast<std::uintptr_t>(p);
    const auto lo = reinterpret_cast<std::uintptr_t>(base);
    return at >= lo && at - lo < bytes && (at - lo) % lineSize == 0;
}

std::optional<double> preemptionFreeRate(const std::vector<double> &unitSeconds, std::uint64_t hopsPerUnit)
{
    if (unitSeconds.empty())
        return std::nullopt;
    std::vector<double> sorted = unitSeconds;
    const std::size_t mid = sorted.size() / 2;
    std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(mid), sorted.end());
    double median = sorted[mid];
    if (sorted.size() % 2 == 0) {
        const double below = *std::max_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(mid));
        median = (median + below) / 2;
    }
    double seconds = 0;
    std::uint64_t units = 0;
    for (const double s : unitSeconds) {
        if (s <= kPreemptedUnit * median) {
            seconds += s;
            ++units;
        }
    }
    return seconds > 0 ? std::optional<double>(double(units) * double(hopsPerUnit) / seconds) : std::nullopt;
}

std::optional<double> threadCpuSeconds()
{
#if defined(_WIN32)
    return std::nullopt;
#else
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0)
        return std::nullopt;
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
#endif
}

double latencyNs(double hopsPerSecond)
{
    return hopsPerSecond > 0 ? 1e9 / hopsPerSecond : 0.0;
}

namespace {

class ChaseThread : public cpu::KernelThread
{
public:
    ChaseThread(const std::byte *base, std::size_t bytes, std::size_t lineSize, std::uint64_t hops)
        : m_base(base), m_bytes(bytes), m_lineSize(lineSize), m_hops(hops), m_at(base)
    {
    }

    void beginWindow() override
    {
        m_unitSeconds.clear();
        m_units = 0;
        m_cpuStart = threadCpuSeconds();
        m_cpuEnd = m_cpuStart;
    }

    // See the header: the thread's own CPU time where the OS keeps it,
    // else the window's units without the preempted ones.
    std::optional<double> windowRate() const override
    {
        if (m_cpuStart && m_cpuEnd && *m_cpuEnd > *m_cpuStart && m_units > 0)
            return double(m_units) * double(m_hops) / (*m_cpuEnd - *m_cpuStart);
        return preemptionFreeRate(m_unitSeconds, m_hops);
    }

    std::uint64_t unit(std::uint64_t) override
    {
        const auto start = std::chrono::steady_clock::now();
        const void *end = chaseHops(cpu::opaqueValue(m_at), cpu::opaqueValue(m_hops));
        m_unitSeconds.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        if (m_cpuStart) {
            m_cpuEnd = threadCpuSeconds();
            ++m_units;
        }
        if (!chaseInRange(end, m_base, m_bytes, m_lineSize))
            throw std::runtime_error("the pointer chase left its buffer: possible memory corruption or miscompile");
        m_at = end;
        return std::uint64_t(static_cast<const std::byte *>(end) - m_base);
    }

private:
    const std::byte *m_base;
    std::size_t m_bytes;
    std::size_t m_lineSize;
    std::uint64_t m_hops;
    const void *m_at;
    std::vector<double> m_unitSeconds;  // this window's units, in order
    std::optional<double> m_cpuStart, m_cpuEnd;  // the thread's CPU time at the window's start / last unit
    std::uint64_t m_units = 0;
};

} // namespace

ChaseKernel::ChaseKernel(const std::byte *base, std::size_t bytes, std::size_t lineSize, std::uint64_t hopsPerUnit)
    : m_base(base), m_bytes(bytes), m_lineSize(lineSize), m_hops(hopsPerUnit)
{
    if (m_hops == 0 || m_hops % kHopsPerIteration != 0)
        throw std::invalid_argument("ChaseKernel: hops per unit must be a positive multiple of 16");
}

std::unique_ptr<cpu::KernelThread> ChaseKernel::makeThread(int) const
{
    return std::make_unique<ChaseThread>(m_base, m_bytes, m_lineSize, m_hops);
}

} // namespace sb::ram
