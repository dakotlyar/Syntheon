// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SynBench's own PRNG (Phase 2 design P10): splitmix64 to expand a seed,
// xoshiro256** to generate (both by Vigna, public domain). Integer-only and
// fully specified, so every OS, compiler and CPU produces the same stream;
// the standard library's engines and distributions are not used anywhere in
// the CPU test, because their output differs between implementations.

#include <cstdint>

namespace sb::cpu {

class SplitMix64
{
public:
    explicit constexpr SplitMix64(std::uint64_t seed) : m_state(seed) {}
    constexpr std::uint64_t next()
    {
        std::uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t m_state;
};

class Xoshiro256ss
{
public:
    // The state from splitmix64(seed), as the authors recommend.
    explicit constexpr Xoshiro256ss(std::uint64_t seed)
    {
        SplitMix64 sm(seed);
        for (std::uint64_t &w : m_s)
            w = sm.next();
    }

    constexpr std::uint64_t next()
    {
        const std::uint64_t result = rotl(m_s[1] * 5, 7) * 9;
        const std::uint64_t t = m_s[1] << 17;
        m_s[2] ^= m_s[0];
        m_s[3] ^= m_s[1];
        m_s[1] ^= m_s[2];
        m_s[0] ^= m_s[3];
        m_s[2] ^= t;
        m_s[3] = rotl(m_s[3], 45);
        return result;
    }

    // A value in [0, bound), bound > 0, without the modulo bias (Lemire's
    // multiply-shift with rejection; 128-bit product built from 64-bit halves
    // so it is the same on MSVC, which has no __int128).
    constexpr std::uint64_t below(std::uint64_t bound)
    {
        for (;;) {
            const std::uint64_t x = next();
            std::uint64_t hi = 0, lo = 0;
            mul128(x, bound, hi, lo);
            if (lo >= bound || lo >= (0 - bound) % bound)
                return hi;
        }
    }

private:
    static constexpr std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    static constexpr void mul128(std::uint64_t a, std::uint64_t b, std::uint64_t &hi, std::uint64_t &lo)
    {
        const std::uint64_t aLo = a & 0xFFFFFFFFu, aHi = a >> 32, bLo = b & 0xFFFFFFFFu, bHi = b >> 32;
        const std::uint64_t ll = aLo * bLo, lh = aLo * bHi, hl = aHi * bLo, hh = aHi * bHi;
        const std::uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
        lo = (mid << 32) | (ll & 0xFFFFFFFFu);
        hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    }

    std::uint64_t m_s[4] = {};
};

} // namespace sb::cpu
