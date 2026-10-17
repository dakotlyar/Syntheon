// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

namespace sb::isa {
struct CpuFeatures;
}

namespace sb::sysinfo {

// x86 CPUID (x64 builds only; empty elsewhere), decoded by synbench_isa
// (P15). Features are normalised like normalizeX86Flags() and only include
// what the OS has enabled (XGETBV for AVX/AVX-512 state).
struct X86Cpu {
    QString vendor;  // "AuthenticAMD", "GenuineIntel"
    QString brand;   // brand string
    QStringList features;
};
X86Cpu queryX86Cpu();

// System info's view of synbench_isa's decoding: vendor, brand and the
// features in x86FeatureOrder(). queryX86Cpu() is fromIsa() of the live CPUID.
X86Cpu fromIsa(const isa::CpuFeatures &features);

} // namespace sb::sysinfo
