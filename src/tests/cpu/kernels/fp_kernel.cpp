// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/kernels/fp_kernel.h"

#include "tests/cpu/engine/opaque.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/kernel_common.h"
#include "tests/cpu/kernels/prng.h"

#include <bit>
#include <cmath>

SB_KERNEL_TU

namespace sb::cpu {

namespace {

constexpr double kA = -0.925, kB = 3.7;
constexpr float kAf = -0.925f, kBf = 3.7f;

inline double logistic(double z)
{
    return (z * kA + kB) * z;
}
inline float logistic(float z)
{
    return (z * kAf + kBf) * z;
}

// A start value in [1.1, 3.6) from 53 random bits (exact: an integer times
// a power of two, then one correctly rounded multiply-add's worth of steps).
double startValue(std::uint64_t bits)
{
    const double unit = double(bits >> 11) * 0x1p-53;  // [0, 1), exact
    const double scaled = unit * 2.5;
    return scaled + 1.1;
}

} // namespace

SB_NOINLINE std::uint64_t fpUnit(std::uint64_t seed)
{
    opaque(seed);
    double start[17];
    SplitMix64 init(seed ^ 0x464C4F4154494E47ull);  // "FLOATING"
    for (double &v : start) {
        const std::uint64_t bits = init.next();  // one draw per statement
        v = startValue(bits);
    }

    double z0 = start[0], z1 = start[1], z2 = start[2], z3 = start[3];
    double z4 = start[4], z5 = start[5], z6 = start[6], z7 = start[7];
    float w0 = float(start[8]), w1 = float(start[9]), w2 = float(start[10]), w3 = float(start[11]);
    float w4 = float(start[12]), w5 = float(start[13]), w6 = float(start[14]), w7 = float(start[15]);
    double x = start[16];

    SB_NO_VECTOR
    for (std::uint64_t i = 0; i < kFpRounds; ++i) {
        SB_NO_VECTOR
        for (int s = 0; s < 4; ++s) {
            z0 = logistic(z0);
            z1 = logistic(z1);
            z2 = logistic(z2);
            z3 = logistic(z3);
            z4 = logistic(z4);
            z5 = logistic(z5);
            z6 = logistic(z6);
            z7 = logistic(z7);
        }
        const double t = x * 1.5;
        x = std::sqrt(t + z0) / 1.25;
        SB_NO_VECTOR
        for (int s = 0; s < 4; ++s) {
            w0 = logistic(w0);
            w1 = logistic(w1);
            w2 = logistic(w2);
            w3 = logistic(w3);
            w4 = logistic(w4);
            w5 = logistic(w5);
            w6 = logistic(w6);
            w7 = logistic(w7);
        }
    }

    std::uint64_t sum = 0;
    for (const double v : {z0, z1, z2, z3, z4, z5, z6, z7, x})
        sum = foldChecksum(sum, std::bit_cast<std::uint64_t>(v));
    for (const float v : {w0, w1, w2, w3, w4, w5, w6, w7})
        sum = foldChecksum(sum, std::bit_cast<std::uint32_t>(v));
    return sum;
}

std::unique_ptr<KernelThread> FpKernel::makeThread(int) const
{
    struct T : KernelThread {
        std::uint64_t unit(std::uint64_t unitIndex) override { return fpUnit(unitIndex % kSeeds); }
    };
    return std::make_unique<T>();
}

std::optional<std::uint64_t> FpKernel::golden(std::uint64_t seed) const
{
    return golden::kFp[seed % kSeeds];
}

} // namespace sb::cpu
