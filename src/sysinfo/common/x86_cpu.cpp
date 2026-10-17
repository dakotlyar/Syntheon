// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/common/x86_cpu.h"

#include "isa/cpu_features.h"
#include "sysinfo/parsers/text_parsers.h"

#include <QHash>

namespace sb::sysinfo {

X86Cpu fromIsa(const isa::CpuFeatures &f)
{
    using isa::Feature;
    // System info's names for the features it lists (x86FeatureOrder()); the
    // others synbench_isa knows (PCLMULQDQ, VAES...) aren't shown here.
    static const QHash<QString, Feature> byName = {
        {QStringLiteral("sse2"), Feature::Sse2},          {QStringLiteral("sse3"), Feature::Sse3},
        {QStringLiteral("ssse3"), Feature::Ssse3},        {QStringLiteral("sse4.1"), Feature::Sse41},
        {QStringLiteral("sse4.2"), Feature::Sse42},       {QStringLiteral("popcnt"), Feature::Popcnt},
        {QStringLiteral("aes"), Feature::Aes},            {QStringLiteral("f16c"), Feature::F16c},
        {QStringLiteral("fma"), Feature::Fma},            {QStringLiteral("bmi1"), Feature::Bmi1},
        {QStringLiteral("bmi2"), Feature::Bmi2},          {QStringLiteral("avx"), Feature::Avx},
        {QStringLiteral("avx2"), Feature::Avx2},          {QStringLiteral("sha"), Feature::Sha},
        {QStringLiteral("avx512f"), Feature::Avx512F},    {QStringLiteral("avx512dq"), Feature::Avx512Dq},
        {QStringLiteral("avx512cd"), Feature::Avx512Cd},  {QStringLiteral("avx512bw"), Feature::Avx512Bw},
        {QStringLiteral("avx512vl"), Feature::Avx512Vl},  {QStringLiteral("avx512vnni"), Feature::Avx512Vnni},
        {QStringLiteral("avx512bf16"), Feature::Avx512Bf16}, {QStringLiteral("avxvnni"), Feature::AvxVnni},
        {QStringLiteral("amx"), Feature::Amx},
    };
    X86Cpu out;
    out.vendor = QString::fromStdString(f.vendor);
    out.brand = QString::fromStdString(f.brand);
    for (const QString &name : x86FeatureOrder()) {
        const auto it = byName.constFind(name);
        if (it != byName.constEnd() && f.has(*it))
            out.features.append(name);
    }
    return out;
}

X86Cpu queryX86Cpu()
{
#if defined(__x86_64__) || defined(_M_X64)
    // P15: the same CPUID/XGETBV decoding the CPU test dispatches on.
    return fromIsa(isa::decodeX86(isa::readX86Cpuid()));
#else
    return {};
#endif
}

} // namespace sb::sysinfo
