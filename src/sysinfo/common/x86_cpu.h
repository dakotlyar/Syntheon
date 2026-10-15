// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

namespace sb::sysinfo {

// x86 CPUID (x64 builds only; empty elsewhere). Features are normalised like
// normalizeX86Flags() and only include what the OS has enabled (XGETBV for
// AVX/AVX-512 state).
struct X86Cpu {
    QString vendor;  // "AuthenticAMD", "GenuineIntel"
    QString brand;   // brand string
    QStringList features;
};
X86Cpu queryX86Cpu();

} // namespace sb::sysinfo
