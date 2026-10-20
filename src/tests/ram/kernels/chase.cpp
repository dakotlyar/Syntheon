// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/kernels/chase.h"

#include "tests/cpu/engine/opaque.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sb::ram {

std::uint64_t hopsPerUnit(double estimateNs)
{
    if (!(estimateNs > 0) || !std::isfinite(estimateNs))
        estimateNs = 1.0;
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

    std::uint64_t unit(std::uint64_t) override
    {
        const void *end = chaseHops(cpu::opaqueValue(m_at), cpu::opaqueValue(m_hops));
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
