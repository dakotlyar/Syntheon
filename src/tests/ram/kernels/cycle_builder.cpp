// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/kernels/cycle_builder.h"

#include "tests/cpu/kernels/prng.h"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace sb::ram {

namespace {

bool stop(const std::function<bool()> &cancelled)
{
    return cancelled && cancelled();
}

} // namespace

std::uint64_t chaseSeed(std::size_t bytes)
{
    // Any fixed constant per size will do; this one mixes the size in so
    // neighbouring grid points don't share a prefix of their shuffles.
    return 0x5EB0C4A5E0000000ull ^ (std::uint64_t(bytes) * 0x9E3779B97F4A7C15ull);
}

std::optional<std::vector<std::uint32_t>> chaseOrder(std::size_t lines, std::uint64_t seed,
                                                     const std::function<bool()> &cancelled)
{
    if (lines < 2 || std::uint64_t(lines) > (std::uint64_t(1) << 32))
        throw std::invalid_argument("chaseOrder: a cycle needs 2 .. 2^32 lines");
    // Step 1, in chunks so a cancel isn't held up by a 128 MiB fill.
    std::vector<std::uint32_t> order;
    order.reserve(lines);
    for (std::size_t chunk = 0; chunk < lines; chunk += kCycleCancelEvery) {
        if (stop(cancelled))
            return std::nullopt;
        const std::size_t end = std::min(lines, chunk + kCycleCancelEvery);
        for (std::size_t i = chunk; i < end; ++i)
            order.push_back(std::uint32_t(i));
    }
    // Step 2: Sattolo, i = n-1 .. 1, j uniform in [0, i).
    cpu::Xoshiro256ss rng(seed);
    std::size_t sinceCheck = 0;
    for (std::size_t i = lines - 1; i > 0; --i) {
        if (++sinceCheck == kCycleCancelEvery) {
            sinceCheck = 0;
            if (stop(cancelled))
                return std::nullopt;
        }
        const std::size_t j = std::size_t(rng.below(i));
        std::swap(order[i], order[j]);
    }
    return order;
}

CycleBuild buildChaseCycle(std::byte *base, std::size_t bytes, std::size_t lineSize, std::uint64_t seed,
                           const std::function<bool()> &cancelled)
{
    if (lineSize < sizeof(void *) || lineSize % sizeof(void *) != 0 || bytes % lineSize != 0)
        throw std::invalid_argument("buildChaseCycle: the buffer must be whole lines of whole pointers");
    const std::size_t lines = bytes / lineSize;
    std::optional<std::vector<std::uint32_t>> order;
    try {
        order = chaseOrder(lines, seed, cancelled);
    } catch (const std::bad_alloc &) {
        return CycleBuild::NoMemory;
    }
    if (!order)
        return CycleBuild::Cancelled;
    // Step 3.
    const std::vector<std::uint32_t> &perm = *order;
    for (std::size_t chunk = 0; chunk < lines; chunk += kCycleCancelEvery) {
        if (stop(cancelled))
            return CycleBuild::Cancelled;
        const std::size_t end = std::min(lines, chunk + kCycleCancelEvery);
        for (std::size_t i = chunk; i < end; ++i) {
            const std::size_t next = i + 1 == lines ? 0 : i + 1;
            *reinterpret_cast<void **>(base + std::size_t(perm[i]) * lineSize) =
                base + std::size_t(perm[next]) * lineSize;
        }
    }
    return CycleBuild::Ok;
}

} // namespace sb::ram
