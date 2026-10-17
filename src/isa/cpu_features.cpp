// SPDX-License-Identifier: GPL-3.0-or-later
#include "isa/cpu_features.h"

#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#define SB_ISA_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#if defined(__aarch64__) && defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
#define SB_ISA_WIN_ARM 1
#define NOMINMAX
#include <windows.h>
#endif

namespace sb::isa {

namespace {

bool bit(std::uint32_t v, int n)
{
    return (v >> n) & 1u;
}

// Collapses runs of spaces and trims, like QString::simplified().
std::string simplified(const char *s)
{
    std::string out;
    bool space = false;
    for (; *s; ++s) {
        if (*s == ' ' || *s == '\t') {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out += *s;
    }
    return out;
}

// arm64 <asm/hwcap.h> bits (the kernel's uapi values).
constexpr std::uint64_t kHwcapAsimd = 1ull << 1;
constexpr std::uint64_t kHwcapAes = 1ull << 3;
constexpr std::uint64_t kHwcapPmull = 1ull << 4;
constexpr std::uint64_t kHwcapSha1 = 1ull << 5;
constexpr std::uint64_t kHwcapSha2 = 1ull << 6;
constexpr std::uint64_t kHwcapCrc32 = 1ull << 7;
constexpr std::uint64_t kHwcapAtomics = 1ull << 8;
constexpr std::uint64_t kHwcapAsimdHp = 1ull << 10;
constexpr std::uint64_t kHwcapAsimdDp = 1ull << 20;
constexpr std::uint64_t kHwcapSve = 1ull << 22;
constexpr std::uint64_t kHwcap2Sve2 = 1ull << 1;
constexpr std::uint64_t kHwcap2I8mm = 1ull << 13;
constexpr std::uint64_t kHwcap2Bf16 = 1ull << 14;
constexpr std::uint64_t kHwcap2Sme = 1ull << 23;
#if defined(__aarch64__) && defined(__linux__)
// Where the real header is at hand (the ARM64 builds), it must agree.
static_assert(kHwcapAsimd == HWCAP_ASIMD && kHwcapAes == HWCAP_AES && kHwcapPmull == HWCAP_PMULL
              && kHwcapSha1 == HWCAP_SHA1 && kHwcapSha2 == HWCAP_SHA2 && kHwcapCrc32 == HWCAP_CRC32
              && kHwcapAtomics == HWCAP_ATOMICS && kHwcapAsimdHp == HWCAP_ASIMDHP && kHwcapAsimdDp == HWCAP_ASIMDDP
              && kHwcapSve == HWCAP_SVE);
#ifdef HWCAP2_SVE2
static_assert(kHwcap2Sve2 == HWCAP2_SVE2);
#endif
#ifdef HWCAP2_I8MM
static_assert(kHwcap2I8mm == HWCAP2_I8MM);
#endif
#ifdef HWCAP2_BF16
static_assert(kHwcap2Bf16 == HWCAP2_BF16);
#endif
#ifdef HWCAP2_SME
static_assert(kHwcap2Sme == HWCAP2_SME);
#endif
#endif

} // namespace

// --- x86 ---------------------------------------------------------------------

CpuFeatures decodeX86(const X86Cpuid &raw)
{
    CpuFeatures f;
    char vendor[13] = {};
    std::memcpy(vendor, &raw.leaf0.b, 4);
    std::memcpy(vendor + 4, &raw.leaf0.d, 4);
    std::memcpy(vendor + 8, &raw.leaf0.c, 4);
    f.vendor = vendor;
    if (raw.maxExtendedLeaf >= 0x80000004) {
        char brand[49] = {};
        for (int i = 0; i < 3; ++i) {
            std::memcpy(brand + i * 16, &raw.brand[i].a, 4);
            std::memcpy(brand + i * 16 + 4, &raw.brand[i].b, 4);
            std::memcpy(brand + i * 16 + 8, &raw.brand[i].c, 4);
            std::memcpy(brand + i * 16 + 12, &raw.brand[i].d, 4);
        }
        f.brand = simplified(brand);
    }

    const CpuidRegs &l1 = raw.leaf1;
    const bool osxsave = bit(l1.c, 27);
    const std::uint64_t xcr0 = osxsave ? raw.xcr0 : 0;
    const bool avxState = (xcr0 & 0x6) == 0x6;           // XMM + YMM (bits 1, 2)
    const bool avx512State = (xcr0 & 0xE6) == 0xE6;      // + opmask, ZMM_Hi256, Hi16_ZMM (bits 5-7)
    const bool amxState = (xcr0 & 0x60000) == 0x60000;   // TILECFG + TILEDATA

    if (bit(l1.d, 26)) f.set(Feature::Sse2);
    if (bit(l1.c, 0)) f.set(Feature::Sse3);
    if (bit(l1.c, 1)) f.set(Feature::Pclmulqdq);
    if (bit(l1.c, 9)) f.set(Feature::Ssse3);
    if (bit(l1.c, 19)) f.set(Feature::Sse41);
    if (bit(l1.c, 20)) f.set(Feature::Sse42);
    if (bit(l1.c, 23)) f.set(Feature::Popcnt);
    if (bit(l1.c, 25)) f.set(Feature::Aes);
    if (avxState && bit(l1.c, 28)) f.set(Feature::Avx);
    if (avxState && bit(l1.c, 29)) f.set(Feature::F16c);
    if (avxState && bit(l1.c, 12)) f.set(Feature::Fma);

    if (raw.leaf0.a >= 7) {
        const CpuidRegs &l7 = raw.leaf7;
        if (bit(l7.b, 3)) f.set(Feature::Bmi1);
        if (bit(l7.b, 8)) f.set(Feature::Bmi2);
        if (bit(l7.b, 29)) f.set(Feature::Sha);
        if (avxState && bit(l7.b, 5)) f.set(Feature::Avx2);
        if (avxState && bit(l7.c, 9)) f.set(Feature::Vaes);
        if (avxState && bit(l7.c, 10)) f.set(Feature::Vpclmulqdq);
        const bool cpuAvx512 = bit(l7.b, 16) && bit(l7.b, 17) && bit(l7.b, 30) && bit(l7.b, 31);  // F, DQ, BW, VL
        if (avx512State) {
            if (bit(l7.b, 16)) f.set(Feature::Avx512F);
            if (bit(l7.b, 17)) f.set(Feature::Avx512Dq);
            if (bit(l7.b, 28)) f.set(Feature::Avx512Cd);
            if (bit(l7.b, 30)) f.set(Feature::Avx512Bw);
            if (bit(l7.b, 31)) f.set(Feature::Avx512Vl);
            if (bit(l7.c, 11)) f.set(Feature::Avx512Vnni);
        } else if (cpuAvx512) {
            f.avx512WithoutOsState = true;
        }
        if (amxState && bit(l7.d, 24)) f.set(Feature::Amx);
        if (l7.a >= 1) {
            if (avxState && bit(raw.leaf7_1.a, 4)) f.set(Feature::AvxVnni);
            if (avx512State && bit(raw.leaf7_1.a, 5)) f.set(Feature::Avx512Bf16);
        }
    }
    return f;
}

#ifdef SB_ISA_X86

namespace {

CpuidRegs cpuid(std::uint32_t leaf, std::uint32_t sub = 0)
{
    CpuidRegs r;
#if defined(_MSC_VER)
    int out[4];
    __cpuidex(out, int(leaf), int(sub));
    r = {std::uint32_t(out[0]), std::uint32_t(out[1]), std::uint32_t(out[2]), std::uint32_t(out[3])};
#else
    __cpuid_count(leaf, sub, r.a, r.b, r.c, r.d);
#endif
    return r;
}

std::uint64_t xgetbv0()
{
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    std::uint32_t eax = 0, edx = 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<std::uint64_t>(edx) << 32) | eax;
#endif
}

} // namespace

X86Cpuid readX86Cpuid()
{
    X86Cpuid raw;
    raw.leaf0 = cpuid(0);
    raw.leaf1 = cpuid(1);
    if (raw.leaf0.a >= 7) {
        raw.leaf7 = cpuid(7, 0);
        if (raw.leaf7.a >= 1)
            raw.leaf7_1 = cpuid(7, 1);
    }
    raw.maxExtendedLeaf = cpuid(0x80000000).a;
    if (raw.maxExtendedLeaf >= 0x80000004) {
        for (std::uint32_t i = 0; i < 3; ++i)
            raw.brand[i] = cpuid(0x80000002 + i);
    }
    // XGETBV only exists when the OS has set CR4.OSXSAVE (CPUID.1:ECX bit 27).
    if (bit(raw.leaf1.c, 27))
        raw.xcr0 = xgetbv0();
    return raw;
}

#else

X86Cpuid readX86Cpuid()
{
    return {};
}

#endif

// --- ARM64 -------------------------------------------------------------------

CpuFeatures decodeLinuxArmHwcaps(std::uint64_t hwcap, std::uint64_t hwcap2)
{
    CpuFeatures f;
    if (hwcap & kHwcapAsimd) f.set(Feature::Neon);
    if (hwcap & kHwcapAes) f.set(Feature::ArmAes);
    if (hwcap & kHwcapPmull) f.set(Feature::ArmPmull);
    if (hwcap & kHwcapSha1) f.set(Feature::ArmSha1);
    if (hwcap & kHwcapSha2) f.set(Feature::ArmSha2);
    if (hwcap & kHwcapCrc32) f.set(Feature::Crc32);
    if (hwcap & kHwcapAtomics) f.set(Feature::Atomics);
    if (hwcap & kHwcapAsimdHp) f.set(Feature::Fp16);
    if (hwcap & kHwcapAsimdDp) f.set(Feature::DotProd);
    if (hwcap & kHwcapSve) f.set(Feature::Sve);
    if (hwcap2 & kHwcap2Sve2) f.set(Feature::Sve2);
    if (hwcap2 & kHwcap2I8mm) f.set(Feature::I8mm);
    if (hwcap2 & kHwcap2Bf16) f.set(Feature::Bf16);
    if (hwcap2 & kHwcap2Sme) f.set(Feature::Sme);
    return f;
}

CpuFeatures decodeWindowsArm(const WindowsArmFeatures &present)
{
    CpuFeatures f;
    f.set(Feature::Neon);  // architectural on ARM64 Windows
    if (present.crypto) {
        // One Windows flag covers all four (as OpenSSL reads it too).
        f.set(Feature::ArmAes);
        f.set(Feature::ArmPmull);
        f.set(Feature::ArmSha1);
        f.set(Feature::ArmSha2);
    }
    if (present.crc32) f.set(Feature::Crc32);
    if (present.atomics) f.set(Feature::Atomics);
    if (present.dotProd) f.set(Feature::DotProd);
    if (present.sve) f.set(Feature::Sve);
    if (present.sve2) f.set(Feature::Sve2);
    if (present.i8mm) f.set(Feature::I8mm);
    if (present.bf16) f.set(Feature::Bf16);
    if (present.fp16) f.set(Feature::Fp16);
    return f;
}

CpuFeatures detect()
{
#if defined(SB_ISA_X86)
    return decodeX86(readX86Cpuid());
#elif defined(__aarch64__) && defined(__linux__)
    return decodeLinuxArmHwcaps(getauxval(AT_HWCAP), getauxval(AT_HWCAP2));
#elif defined(SB_ISA_WIN_ARM)
    WindowsArmFeatures p;
    const auto has = [](DWORD feature) { return IsProcessorFeaturePresent(feature) != 0; };
#ifdef PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE
    p.crypto = has(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE);
#endif
#ifdef PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE
    p.crc32 = has(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE);
#endif
#ifdef PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE
    p.atomics = has(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE);
#endif
#ifdef PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE
    p.dotProd = has(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE);
#endif
#ifdef PF_ARM_SVE_INSTRUCTIONS_AVAILABLE
    p.sve = has(PF_ARM_SVE_INSTRUCTIONS_AVAILABLE);
#endif
#ifdef PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE
    p.sve2 = has(PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE);
#endif
#ifdef PF_ARM_V82_I8MM_INSTRUCTIONS_AVAILABLE
    p.i8mm = has(PF_ARM_V82_I8MM_INSTRUCTIONS_AVAILABLE);
#endif
#ifdef PF_ARM_V86_BF16_INSTRUCTIONS_AVAILABLE
    p.bf16 = has(PF_ARM_V86_BF16_INSTRUCTIONS_AVAILABLE);
#endif
#ifdef PF_ARM_V82_FP16_INSTRUCTIONS_AVAILABLE
    p.fp16 = has(PF_ARM_V82_FP16_INSTRUCTIONS_AVAILABLE);
#endif
    return decodeWindowsArm(p);
#else
    return {};
#endif
}

// --- the SIMD path ------------------------------------------------------------

SimdPath bestSimdPath(const CpuFeatures &f, bool sveInBuild)
{
    if (f.has(Feature::Avx512F) && f.has(Feature::Avx512Bw) && f.has(Feature::Avx512Dq) && f.has(Feature::Avx512Vl))
        return SimdPath::Avx512;
    if (f.has(Feature::Avx2) && f.has(Feature::Fma))
        return SimdPath::Avx2Fma;
    if (f.has(Feature::Sse42))
        return SimdPath::Sse42;
    if (sveInBuild && f.has(Feature::Sve))
        return SimdPath::Sve;
    if (f.has(Feature::Neon))
        return SimdPath::Neon;
    return SimdPath::None;
}

const char *simdPathName(SimdPath p)
{
    switch (p) {
    case SimdPath::Avx512: return "AVX-512 (512-bit)";
    case SimdPath::Avx2Fma: return "AVX2 + FMA (256-bit)";
    case SimdPath::Sse42: return "SSE4.2 (128-bit)";
    case SimdPath::Sve: return "SVE";
    case SimdPath::Neon: return "NEON (128-bit)";
    case SimdPath::None: break;
    }
    return "none";
}

std::string describeSimdChoice(const CpuFeatures &f, bool sveInBuild)
{
    const SimdPath p = bestSimdPath(f, sveInBuild);
    std::string s = std::string(simdPathName(p)) + " path selected";
    switch (p) {
    case SimdPath::Avx512:
        return s + " (F, BW, DQ, VL; OS ZMM state on)";
    case SimdPath::Avx2Fma:
        if (f.avx512WithoutOsState)
            return s + " (the CPU has AVX-512, but the OS doesn't save its ZMM state)";
        return s + " (no AVX-512 F/BW/DQ/VL)";
    case SimdPath::Sse42:
        return s + " (no AVX2 + FMA with OS YMM state)";
    case SimdPath::Sve:
        return s + " (SVE present)";
    case SimdPath::Neon:
        if (f.has(Feature::Sve) && !sveInBuild)
            return s + " (the CPU has SVE, but this build has no SVE path)";
        return s + " (no SVE)";
    case SimdPath::None:
        break;
    }
    return s + " (no SIMD path for this CPU)";
}

} // namespace sb::isa
