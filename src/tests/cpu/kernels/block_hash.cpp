// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/kernels/block_hash.h"

#include "tests/cpu/kernels/kernel_common.h"

#include <cstddef>
#include <cstring>

SB_KERNEL_TU

namespace sb::cpu {

namespace {

constexpr std::uint64_t kMul = 0x9E3779B97F4A7C15ull;

constexpr std::uint64_t rotl(std::uint64_t x, unsigned n)
{
    return (x << n) | (x >> (64 - n));
}

inline std::uint64_t step(std::uint64_t h, std::uint64_t w)
{
    return rotl((h ^ w) * kMul, 31);
}

// splitmix64's finaliser: every input bit reaches every output bit.
constexpr std::uint64_t avalanche(std::uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace

SB_NOINLINE std::uint64_t blockHash(std::span<const std::uint8_t> data)
{
    std::uint64_t h[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::uint8_t *p = data.data();
    const std::size_t n = data.size();
    std::size_t i = 0;
    SB_NO_VECTOR
    for (; i + 64 <= n; i += 64) {
        std::uint64_t w[8];
        std::memcpy(w, p + i, 64);
        h[0] = step(h[0], w[0]);
        h[1] = step(h[1], w[1]);
        h[2] = step(h[2], w[2]);
        h[3] = step(h[3], w[3]);
        h[4] = step(h[4], w[4]);
        h[5] = step(h[5], w[5]);
        h[6] = step(h[6], w[6]);
        h[7] = step(h[7], w[7]);
    }
    SB_NO_VECTOR
    for (; i < n; ++i)
        h[0] = step(h[0], p[i]);
    std::uint64_t sum = n;
    SB_NO_VECTOR
    for (int l = 0; l < 8; ++l)
        sum = foldChecksum(sum, h[l]);
    return avalanche(sum);
}

} // namespace sb::cpu
