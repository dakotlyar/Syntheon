// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/kernels/chase.h"

#include "tests/cpu/engine/opaque.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
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

std::optional<double> cycleCounterRate(const std::vector<CycleUnit> &units)
{
    std::vector<double> ratios;
    ratios.reserve(units.size());
    for (const CycleUnit &u : units) {
        if (u.seconds > 0 && u.cycles > 0)
            ratios.push_back(u.cycles / u.seconds);
    }
    if (ratios.empty())
        return std::nullopt;
    const auto at = std::size_t(0.9 * double(ratios.size() - 1));
    std::nth_element(ratios.begin(), ratios.begin() + std::ptrdiff_t(at), ratios.end());
    return ratios[at];
}

std::optional<double> cycleTimedRate(const std::vector<CycleUnit> &units, std::uint64_t hopsPerUnit, double carriedRate,
                                     double *rateUsed, double borrowBelow)
{
    std::optional<double> rate = cycleCounterRate(units);
    if (!rate)
        return std::nullopt;
    // A window whose units were all preempted reads its counter rate low:
    // it borrows the earlier rate (the threshold per architecture, see
    // kBorrowBelow in the header).
    if (carriedRate > 0 && *rate < borrowBelow * carriedRate)
        rate = carriedRate;
    if (rateUsed)
        *rateUsed = *rate;
    double cycles = 0;
    std::uint64_t used = 0;
    for (const CycleUnit &u : units) {
        if (u.seconds > 0 && u.cycles > 0) {
            cycles += u.cycles;
            ++used;
        }
    }
    const double seconds = cycles / *rate;
    return seconds > 0 ? std::optional<double>(double(used) * double(hopsPerUnit) / seconds) : std::nullopt;
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
        m_cycleUnits.clear();
        m_cyclesOk = true;
        m_units = 0;
        m_cpuStart = threadCpuSeconds();
        m_cpuEnd = m_cpuStart;
    }

    // See the header: the thread's own time (Linux: its CPU time; Windows:
    // its calibrated cycle count), else the units without the preempted ones.
    std::optional<double> windowRate() const override
    {
        if (m_cpuStart && m_cpuEnd && *m_cpuEnd > *m_cpuStart && m_units > 0)
            return double(m_units) * double(m_hops) / (*m_cpuEnd - *m_cpuStart);
        if (m_cyclesOk && !m_cycleUnits.empty()) {
            double used = 0;
            if (const auto rate = cycleTimedRate(m_cycleUnits, m_hops, m_bestCycleRate, &used)) {
                m_bestCycleRate = std::max(m_bestCycleRate, used);  // carried to the measurement's later windows
                return rate;
            }
        }
        return preemptionFreeRate(m_unitSeconds, m_hops);
    }

    std::uint64_t unit(std::uint64_t) override
    {
        // The unit's wall time is kept on every path (two clock reads per
        // ~2 ms unit): the fallback if the thread's own time fails.
#if defined(_WIN32)
        ULONG64 c0 = 0, c1 = 0;
        const bool ok0 = QueryThreadCycleTime(GetCurrentThread(), &c0) != 0;
#endif
        const auto start = std::chrono::steady_clock::now();
        const void *end = chaseHops(cpu::opaqueValue(m_at), cpu::opaqueValue(m_hops));
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        m_unitSeconds.push_back(seconds);
        if (m_cpuStart) {
            // Linux: the thread's CPU time. A failed read ends that for the
            // window (AP-5 L, round 2): the wall-time units take over.
            m_cpuEnd = threadCpuSeconds();
            if (!m_cpuEnd)
                m_cpuStart.reset();
            ++m_units;
        }
#if defined(_WIN32)
        const bool ok1 = QueryThreadCycleTime(GetCurrentThread(), &c1) != 0;
        if (ok0 && ok1 && c1 >= c0)
            m_cycleUnits.push_back({double(c1 - c0), seconds});
        else
            m_cyclesOk = false;
#endif
        return finishUnit(end);
    }

private:
    std::uint64_t finishUnit(const void *end)
    {
        if (!chaseInRange(end, m_base, m_bytes, m_lineSize))
            throw std::runtime_error("the pointer chase left its buffer: possible memory corruption or miscompile");
        m_at = end;
        return std::uint64_t(static_cast<const std::byte *>(end) - m_base);
    }

    const std::byte *m_base;
    std::size_t m_bytes;
    std::size_t m_lineSize;
    std::uint64_t m_hops;
    const void *m_at;
    std::vector<double> m_unitSeconds;  // this window's units, in order (the fallback)
    std::vector<CycleUnit> m_cycleUnits;  // Windows: each unit's cycles and wall time
    bool m_cyclesOk = true;               // every QueryThreadCycleTime this window worked
    mutable double m_bestCycleRate = 0;   // the highest calibrated counter rate of this measurement's windows
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
