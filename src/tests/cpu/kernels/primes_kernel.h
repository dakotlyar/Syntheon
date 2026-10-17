// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The primes kernel (Phase 2 design §5.2, D3): `fp.primes`. One unit, for a
// seed s in 0..63, is a segmented Sieve of Eratosthenes over the 2^20
// numbers [10^12 + s * 2^20, 10^12 + (s + 1) * 2^20): an odd-only bitset of
// 2^19 bits (64 KiB, L1/L2-resident), crossed off with the base primes, then
// counted. The rate is numbers sieved per second (2^20 per unit).
//
// The base primes are the odd primes up to the square root of the last
// number of the last segment, 1,000,033 (the design's "up to 10^6" rounded
// up: 1,000,003 and 1,000,033 have squares inside the range). They are
// computed once per kernel and shared read-only by every thread; each
// thread has its own bitset.
//
// The checksum is the segment's prime count; golden/primes.inc holds it for
// each seed, cross-checked by tst_cpu_kernels with Miller-Rabin on four
// segments.

#include "tests/cpu/engine/kernel.h"

#include <cstdint>
#include <span>
#include <vector>

namespace sb::cpu {

inline constexpr std::uint64_t kPrimesBase = 1000000000000ull;  // 10^12
inline constexpr std::uint64_t kPrimesSegment = 1u << 20;
inline constexpr std::size_t kPrimesBitsetWords = kPrimesSegment / 2 / 64;  // 8192

// The first number of segment `seed`.
constexpr std::uint64_t primesSegmentStart(std::uint64_t seed)
{
    return kPrimesBase + seed * kPrimesSegment;
}

// The odd primes up to isqrt(the last number of segment 63).
std::vector<std::uint32_t> primesBase();

// One unit: the number of primes in segment `seed`, sieved in `bitset`
// (kPrimesBitsetWords words) with `basePrimes`.
std::uint64_t primesUnit(std::uint64_t seed, std::span<const std::uint32_t> basePrimes, std::uint64_t *bitset);

class PrimesKernel : public Kernel
{
public:
    PrimesKernel();
    std::string name() const override { return "Primes"; }
    double workPerUnit() const override { return double(kPrimesSegment); }
    // P14: two numbers sieved per cycle per thread (it does about 0.2).
    double maxWorkPerCyclePerThread() const override { return 2; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;

private:
    std::vector<std::uint32_t> m_basePrimes;
};

} // namespace sb::cpu
