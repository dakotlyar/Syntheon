// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/common/x86_cpu.h"

#include "sysinfo/parsers/text_parsers.h"

#include <QSet>

#if defined(__x86_64__) || defined(_M_X64)
#define SB_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#include <cstring>

namespace sb::sysinfo {

#ifdef SB_X86

namespace {

struct Regs {
    unsigned a = 0, b = 0, c = 0, d = 0;
};

Regs cpuid(unsigned leaf, unsigned sub = 0)
{
    Regs r;
#if defined(_MSC_VER)
    int out[4];
    __cpuidex(out, int(leaf), int(sub));
    r = {unsigned(out[0]), unsigned(out[1]), unsigned(out[2]), unsigned(out[3])};
#else
    __cpuid_count(leaf, sub, r.a, r.b, r.c, r.d);
#endif
    return r;
}

unsigned long long xgetbv0()
{
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    unsigned eax = 0, edx = 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<unsigned long long>(edx) << 32) | eax;
#endif
}

bool bit(unsigned v, int n)
{
    return (v >> n) & 1u;
}

} // namespace

X86Cpu queryX86Cpu()
{
    X86Cpu out;
    const Regs l0 = cpuid(0);
    const unsigned maxLeaf = l0.a;
    char vendor[13] = {};
    std::memcpy(vendor, &l0.b, 4);
    std::memcpy(vendor + 4, &l0.d, 4);
    std::memcpy(vendor + 8, &l0.c, 4);
    out.vendor = QString::fromLatin1(vendor);

    if (cpuid(0x80000000).a >= 0x80000004) {
        char brand[49] = {};
        for (unsigned i = 0; i < 3; ++i) {
            const Regs r = cpuid(0x80000002 + i);
            std::memcpy(brand + i * 16, &r.a, 4);
            std::memcpy(brand + i * 16 + 4, &r.b, 4);
            std::memcpy(brand + i * 16 + 8, &r.c, 4);
            std::memcpy(brand + i * 16 + 12, &r.d, 4);
        }
        out.brand = QString::fromLatin1(brand).simplified();
    }

    QSet<QString> f;
    const Regs l1 = cpuid(1);
    const bool osxsave = bit(l1.c, 27);
    const unsigned long long xcr0 = osxsave ? xgetbv0() : 0;
    const bool avxState = (xcr0 & 0x6) == 0x6;            // XMM + YMM
    const bool avx512State = (xcr0 & 0xE6) == 0xE6;       // + opmask, ZMM
    const bool amxState = (xcr0 & 0x60000) == 0x60000;    // TILECFG + TILEDATA

    if (bit(l1.d, 26)) f.insert(QStringLiteral("sse2"));
    if (bit(l1.c, 0)) f.insert(QStringLiteral("sse3"));
    if (bit(l1.c, 9)) f.insert(QStringLiteral("ssse3"));
    if (bit(l1.c, 19)) f.insert(QStringLiteral("sse4.1"));
    if (bit(l1.c, 20)) f.insert(QStringLiteral("sse4.2"));
    if (bit(l1.c, 23)) f.insert(QStringLiteral("popcnt"));
    if (bit(l1.c, 25)) f.insert(QStringLiteral("aes"));
    if (avxState && bit(l1.c, 28)) f.insert(QStringLiteral("avx"));
    if (avxState && bit(l1.c, 29)) f.insert(QStringLiteral("f16c"));
    if (avxState && bit(l1.c, 12)) f.insert(QStringLiteral("fma"));

    if (maxLeaf >= 7) {
        const Regs l7 = cpuid(7, 0);
        if (bit(l7.b, 3)) f.insert(QStringLiteral("bmi1"));
        if (bit(l7.b, 8)) f.insert(QStringLiteral("bmi2"));
        if (bit(l7.b, 29)) f.insert(QStringLiteral("sha"));
        if (avxState && bit(l7.b, 5)) f.insert(QStringLiteral("avx2"));
        if (avx512State) {
            if (bit(l7.b, 16)) f.insert(QStringLiteral("avx512f"));
            if (bit(l7.b, 17)) f.insert(QStringLiteral("avx512dq"));
            if (bit(l7.b, 28)) f.insert(QStringLiteral("avx512cd"));
            if (bit(l7.b, 30)) f.insert(QStringLiteral("avx512bw"));
            if (bit(l7.b, 31)) f.insert(QStringLiteral("avx512vl"));
            if (bit(l7.c, 11)) f.insert(QStringLiteral("avx512vnni"));
        }
        if (amxState && bit(l7.d, 24)) f.insert(QStringLiteral("amx"));
        if (l7.a >= 1) {
            const Regs l71 = cpuid(7, 1);
            if (avxState && bit(l71.a, 4)) f.insert(QStringLiteral("avxvnni"));
            if (avx512State && bit(l71.a, 5)) f.insert(QStringLiteral("avx512bf16"));
        }
    }

    for (const QString &name : x86FeatureOrder()) {
        if (f.contains(name))
            out.features.append(name);
    }
    return out;
}

#else

X86Cpu queryX86Cpu()
{
    return {};
}

#endif

} // namespace sb::sysinfo
