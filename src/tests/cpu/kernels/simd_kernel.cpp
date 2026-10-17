// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/kernels/simd_kernel.h"

#include "tests/cpu/engine/opaque.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/kernel_common.h"
#include "tests/cpu/kernels/prng.h"

#include <bit>
#include <stdexcept>

SB_KERNEL_TU

namespace sb::cpu {

namespace {

using namespace simd;

constexpr std::size_t kASize = std::size_t(kGemmM) * kGemmK;
constexpr std::size_t kBSize = std::size_t(kGemmBRows) * kGemmN;
constexpr std::size_t kXSize = kDotLength;
constexpr std::size_t kYSize = std::size_t(kDotLength) + kDotShifts;

#if defined(SB_SIMD_SVE)
static_assert(isa::kSveInBuild, "the SVE TU is built exactly where synbench_isa expects it");
#else
static_assert(!isa::kSveInBuild, "the SVE TU is built exactly where synbench_isa expects it");
#endif

} // namespace

SimdInputs::SimdInputs() : m_a(kASize * kSeeds), m_b(kBSize * kSeeds), m_x(kXSize * kSeeds), m_y(kYSize * kSeeds)
{
    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        Xoshiro256ss rng(s ^ 0x53494D4444415441ull);  // "SIMDDATA"; one draw per statement
        for (std::size_t i = 0; i < kASize; ++i) {
            const std::uint64_t v = rng.below(5);
            m_a[s * kASize + i] = float(int(v) - 2);
        }
        for (std::size_t i = 0; i < kBSize; ++i) {
            const std::uint64_t v = rng.below(5);
            m_b[s * kBSize + i] = float(int(v) - 2);
        }
        for (std::size_t i = 0; i < kXSize; ++i) {
            const std::uint64_t v = rng.below(256);
            m_x[s * kXSize + i] = std::int16_t(int(v) - 128);
        }
        for (std::size_t i = 0; i < kYSize; ++i) {
            const std::uint64_t v = rng.below(256);
            m_y[s * kYSize + i] = std::int16_t(int(v) - 128);
        }
    }
}

simd::Input SimdInputs::forSeed(std::uint64_t seed) const
{
    const std::size_t s = std::size_t(seed % kSeeds);
    return {m_a.data() + s * kASize, m_b.data() + s * kBSize, m_x.data() + s * kXSize, m_y.data() + s * kYSize};
}

SB_NOINLINE void simdReferenceUnit(const simd::Input &in, simd::Output &out)
{
    const float *a = in.a;
    const float *b = in.b;
    opaque(a);
    opaque(b);
    SB_NO_VECTOR
    for (float &c : out.c)
        c = 0;
    SB_NO_VECTOR
    for (int r = 0; r < kGemmReps; ++r) {
        const float *br = b + (r % 16) * kGemmN;
        SB_NO_VECTOR
        for (int i = 0; i < kGemmM; ++i) {
            SB_NO_VECTOR
            for (int k = 0; k < kGemmK; ++k) {
                const float aik = a[i * kGemmK + k];
                SB_NO_VECTOR
                for (int j = 0; j < kGemmN; ++j)
                    out.c[i * kGemmN + j] += aik * br[k * kGemmN + j];
            }
        }
    }
    const std::int16_t *x = in.x;
    opaque(x);
    std::int64_t sum = 0, weighted = 0;
    SB_NO_VECTOR
    for (int p = 0; p < kDotReps; ++p) {
        const std::int16_t *y = in.y + p % kDotShifts;
        std::int32_t d = 0;
        SB_NO_VECTOR
        for (int i = 0; i < kDotLength; ++i)
            d += std::int32_t(x[i]) * std::int32_t(y[i]);
        sum += d;
        weighted += std::int64_t(p + 1) * d;
    }
    out.dotSum = sum;
    out.dotWeighted = weighted;
    out.vectorBits = 0;
}

std::uint64_t simdChecksum(const simd::Output &out)
{
    std::uint64_t sum = 0;
    for (const float c : out.c)
        sum = foldChecksum(sum, std::bit_cast<std::uint32_t>(c));
    sum = foldChecksum(sum, std::uint64_t(out.dotSum));
    return foldChecksum(sum, std::uint64_t(out.dotWeighted));
}

SimdUnitFn simdUnitFor(isa::SimdPath path)
{
    switch (path) {
#if defined(__x86_64__) || defined(_M_X64)
    case isa::SimdPath::Sse42: return sb_simd_sse42_unit;
    case isa::SimdPath::Avx2Fma: return sb_simd_avx2_unit;
    case isa::SimdPath::Avx512: return sb_simd_avx512_unit;
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    case isa::SimdPath::Neon: return sb_simd_neon_unit;
#endif
#if defined(SB_SIMD_SVE)
    case isa::SimdPath::Sve: return sb_simd_sve_unit;
#endif
    default: return nullptr;
    }
}

std::string simdPathLabel(isa::SimdPath path, std::uint32_t vectorBits)
{
    if (path == isa::SimdPath::Sve)
        return "SVE (" + std::to_string(vectorBits) + "-bit, unvalidated)";
    return isa::simdPathName(path);
}

SimdChoice chooseSimdPath(const isa::CpuFeatures &features, const SimdInputs &inputs)
{
    SimdChoice c;
    c.path = isa::bestSimdPath(features);
    c.note = isa::describeSimdChoice(features);
    switch (c.path) {
    case isa::SimdPath::Avx512: c.vectorBits = 512; break;
    case isa::SimdPath::Avx2Fma: c.vectorBits = 256; break;
    default: c.vectorBits = 128; break;
    }
    if (c.path == isa::SimdPath::Sve) {
        // D15: the SVE path has never run anywhere; one unit against the
        // golden table before it is trusted, else NEON.
        simd::Output out{};
        const simd::Input in = inputs.forSeed(0);
        simdUnitFor(isa::SimdPath::Sve)(&in, &out);
        if (simdChecksum(out) == golden::kSimd[0]) {
            c.vectorBits = out.vectorBits;
            c.note += "; its self-check against the golden table passed (unvalidated: no SVE machine has run it yet)";
        } else {
            c.path = isa::SimdPath::Neon;
            c.vectorBits = 128;
            c.warning = true;
            c.note = "The SVE path failed its self-check against the golden table, so the NEON path runs instead";
        }
    }
    if (!simdUnitFor(c.path)) {
        c.path = isa::SimdPath::None;
        c.vectorBits = 0;
        c.warning = true;
        c.note = "No SIMD path in this build for this CPU";
    }
    c.label = simdPathLabel(c.path, c.vectorBits);
    return c;
}

SimdKernel::SimdKernel(const SimdInputs &inputs, isa::SimdPath path, std::uint32_t vectorBits)
    : m_inputs(inputs), m_path(path), m_vectorBits(vectorBits), m_unit(simdUnitFor(path))
{
}

double SimdKernel::maxWorkPerCyclePerThread() const
{
    const double lanes = double(m_vectorBits) / 32;
    switch (m_path) {
    case isa::SimdPath::Sse42: return 4 * lanes;   // two multiply and two add pipes, no FMA
    case isa::SimdPath::Avx2Fma:
    case isa::SimdPath::Avx512: return 2 * 2 * lanes;  // two FMA pipes
    case isa::SimdPath::Neon:
    case isa::SimdPath::Sve: return 4 * 2 * lanes;     // four FMA pipes
    default: return 4;
    }
}

std::unique_ptr<KernelThread> SimdKernel::makeThread(int) const
{
    struct T : KernelThread {
        const SimdInputs *inputs;
        SimdUnitFn fn;
        simd::Output out{};
        std::uint64_t unit(std::uint64_t unitIndex) override
        {
            const simd::Input in = inputs->forSeed(unitIndex % kSeeds);
            fn(&in, &out);
            return simdChecksum(out);
        }
    };
    if (!m_unit)
        throw std::runtime_error(std::string("no ") + isa::simdPathName(m_path) + " path in this build");
    auto t = std::make_unique<T>();
    t->inputs = &m_inputs;
    t->fn = m_unit;
    return t;
}

std::optional<std::uint64_t> SimdKernel::golden(std::uint64_t seed) const
{
    return golden::kSimd[seed % kSeeds];
}

} // namespace sb::cpu
