// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/kernels/primes_kernel.h"

#include "tests/cpu/engine/opaque.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/kernel_common.h"

#include <bit>
#include <cstddef>

SB_KERNEL_TU

namespace sb::cpu {

namespace {

constexpr std::uint64_t kBits = kPrimesSegment / 2;  // odd numbers per segment

// floor(sqrt(n)), integers only.
constexpr std::uint64_t isqrt(std::uint64_t n)
{
    std::uint64_t r = 0;
    for (std::uint64_t bit = 1ull << 62; bit != 0; bit >>= 2) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return r;
}

constexpr std::uint64_t kLastNumber = primesSegmentStart(kSeeds) - 1;
constexpr std::uint64_t kBaseLimit = isqrt(kLastNumber);
static_assert(kBaseLimit == 1000033, "the base primes reach 1,000,033");

} // namespace

std::vector<std::uint32_t> primesBase()
{
    // A plain odd-only sieve up to kBaseLimit.
    const std::uint64_t n = kBaseLimit + 1;
    std::vector<bool> composite(std::size_t(n), false);
    std::vector<std::uint32_t> primes;
    for (std::uint64_t i = 3; i < n; i += 2) {
        if (composite[std::size_t(i)])
            continue;
        primes.push_back(std::uint32_t(i));
        for (std::uint64_t j = i * i; j < n; j += 2 * i)
            composite[std::size_t(j)] = true;
    }
    return primes;
}

SB_NOINLINE std::uint64_t primesUnit(std::uint64_t seed, std::span<const std::uint32_t> basePrimes,
                                     std::uint64_t *bitset)
{
    opaque(seed);
    opaque(bitset);
    const std::uint32_t *primes = basePrimes.data();
    opaque(primes);
    const std::size_t count = basePrimes.size();

    // Bit k stands for the odd number lo + 1 + 2k; lo is even.
    const std::uint64_t lo = primesSegmentStart(seed);
    SB_NO_VECTOR
    for (std::size_t w = 0; w < kPrimesBitsetWords; ++w)
        bitset[w] = 0;
    SB_NO_VECTOR
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t p = primes[i];
        // The first odd multiple of p at or after lo + 1. Every base prime is
        // below lo, so p itself is never crossed off.
        std::uint64_t m = (lo + p) / p * p;
        if ((m & 1) == 0)
            m += p;
        SB_NO_VECTOR
        for (std::uint64_t k = (m - lo - 1) / 2; k < kBits; k += p)
            bitset[k >> 6] |= 1ull << (k & 63);
    }
    std::uint64_t primesFound = 0;
    SB_NO_VECTOR
    for (std::size_t w = 0; w < kPrimesBitsetWords; ++w)
        primesFound += std::uint64_t(std::popcount(~bitset[w]));
    return primesFound;
}

PrimesKernel::PrimesKernel() : m_basePrimes(primesBase()) {}

std::unique_ptr<KernelThread> PrimesKernel::makeThread(int) const
{
    struct T : KernelThread {
        std::span<const std::uint32_t> base;
        std::vector<std::uint64_t> bitset = std::vector<std::uint64_t>(kPrimesBitsetWords, 0);  // touched here
        std::uint64_t unit(std::uint64_t unitIndex) override
        {
            return primesUnit(unitIndex % kSeeds, base, bitset.data());
        }
    };
    auto t = std::make_unique<T>();
    t->base = m_basePrimes;
    return t;
}

std::optional<std::uint64_t> PrimesKernel::golden(std::uint64_t seed) const
{
    return golden::kPrimes[seed % kSeeds];
}

} // namespace sb::cpu
