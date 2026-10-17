// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the CPU and the OS support (Phase 2 design §1, P9, P15). Plain C++20,
// no Qt: it is linked into the CPU test, System info and the Qt-free crypto
// worker, and is the one source of truth for "AVX-512 present".
//
// x86: CPUID, with the vector state the OS has enabled (XCR0 via XGETBV): a
// CPU's AVX/AVX-512 bits only count when the OS saves the registers.
// ARM64: Linux hwcaps (getauxval), Windows IsProcessorFeaturePresent.
// The decoders are pure functions of the raw values, so tests inject bits.

#include <bitset>
#include <cstdint>
#include <string>

namespace sb::isa {

enum class Feature : unsigned {
    // x86
    Sse2, Sse3, Ssse3, Sse41, Sse42, Popcnt, Aes, Pclmulqdq, F16c, Fma, Bmi1, Bmi2,
    Avx, Avx2, Sha, Vaes, Vpclmulqdq,
    Avx512F, Avx512Dq, Avx512Cd, Avx512Bw, Avx512Vl, Avx512Vnni, Avx512Bf16, AvxVnni, Amx,
    // ARM64
    Neon, ArmAes, ArmPmull, ArmSha1, ArmSha2, Crc32, Atomics, DotProd, Fp16, Sve, Sve2, I8mm, Bf16, Sme,
    Count
};

struct CpuFeatures {
    std::bitset<static_cast<unsigned>(Feature::Count)> bits;
    std::string vendor;  // x86 CPUID vendor ("AuthenticAMD", "GenuineIntel"); empty on ARM
    std::string brand;   // x86 brand string, whitespace collapsed; empty on ARM
    // x86: the CPU reports AVX-512 F/BW/DQ/VL but the OS doesn't save the
    // ZMM / opmask state (XCR0 bits 5-7), so AVX-512 is unusable and absent
    // from `bits`. Kept to explain the SIMD choice in the log.
    bool avx512WithoutOsState = false;

    bool has(Feature f) const { return bits.test(static_cast<unsigned>(f)); }
    void set(Feature f) { bits.set(static_cast<unsigned>(f)); }
};

// --- x86 -------------------------------------------------------------------
struct CpuidRegs {
    std::uint32_t a = 0, b = 0, c = 0, d = 0;
};

// The CPUID leaves the decoder reads, as the hardware returned them.
struct X86Cpuid {
    CpuidRegs leaf0;        // max leaf; vendor in b, d, c
    CpuidRegs leaf1;
    CpuidRegs leaf7;        // sub-leaf 0
    CpuidRegs leaf7_1;      // sub-leaf 1 (only if leaf7.a >= 1)
    std::uint32_t maxExtendedLeaf = 0;
    CpuidRegs brand[3];     // 0x80000002..4
    std::uint64_t xcr0 = 0; // XGETBV(0); 0 when OSXSAVE is off
};

CpuFeatures decodeX86(const X86Cpuid &raw);
// The real CPUID/XGETBV (x64 builds; elsewhere an empty X86Cpuid).
X86Cpuid readX86Cpuid();

// --- ARM64 -----------------------------------------------------------------
// Linux AT_HWCAP / AT_HWCAP2 values (the bit layout of <asm/hwcap.h> for
// arm64, which this file defines itself so it also compiles elsewhere).
CpuFeatures decodeLinuxArmHwcaps(std::uint64_t hwcap, std::uint64_t hwcap2);

// Windows on ARM: the answers of IsProcessorFeaturePresent.
struct WindowsArmFeatures {
    bool crypto = false;   // PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE (AES, PMULL, SHA-1, SHA-256)
    bool crc32 = false;    // PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE
    bool atomics = false;  // PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE
    bool dotProd = false;  // PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE
    bool sve = false;      // PF_ARM_SVE_INSTRUCTIONS_AVAILABLE
    bool sve2 = false;
    bool i8mm = false;
    bool bf16 = false;
};
CpuFeatures decodeWindowsArm(const WindowsArmFeatures &present);

// This machine, as the running process sees it.
CpuFeatures detect();

// --- the SIMD path (P9) ----------------------------------------------------
// The widest path the CPU and the OS support: AVX-512 (F+BW+DQ+VL) > AVX2+FMA
// > SSE4.2 on x86; SVE > NEON on ARM64 (SVE only where this build has an SVE
// kernel: Linux, since MSVC has no SVE intrinsics). Feature-based, not timed.
enum class SimdPath { None, Sse42, Avx2Fma, Avx512, Neon, Sve };

// Whether this build has SVE kernels (Linux ARM64).
#if defined(__aarch64__) && defined(__linux__)
inline constexpr bool kSveInBuild = true;
#else
inline constexpr bool kSveInBuild = false;
#endif

SimdPath bestSimdPath(const CpuFeatures &f, bool sveInBuild = kSveInBuild);
// "AVX-512 (512-bit)", "AVX2 + FMA (256-bit)", "SSE4.2 (128-bit)", "NEON (128-bit)",
// "SVE" (the vector length is only known at run time), "none".
const char *simdPathName(SimdPath p);
// The log line: which path and why, e.g. "AVX-512 path selected (F, BW, DQ,
// VL; OS ZMM state on)".
std::string describeSimdChoice(const CpuFeatures &f, bool sveInBuild = kSveInBuild);

} // namespace sb::isa
