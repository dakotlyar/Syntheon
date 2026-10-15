// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace sb::sysinfo {

// Pure parsers over text captured from a machine, so they can be tested with
// fixture files from every machine (§5). No I/O here.

// "KEY=value" / "KEY="quoted value"" lines (os-release).
QMap<QString, QString> parseOsRelease(const QString &text);

// /proc/meminfo "MemTotal:  65432100 kB" -> bytes; 0 if missing.
qint64 parseMemTotal(const QString &text);

struct ArmCoreId {
    int implementer = -1, variant = -1, part = -1, revision = -1;
    bool operator==(const ArmCoreId &) const = default;
};

// One /proc/cpuinfo, x86 or ARM.
struct CpuInfoText {
    QString vendorId;   // x86 "AuthenticAMD"
    QString modelName;  // x86 "model name"; ARM usually has none
    QStringList flags;  // x86 "flags" or ARM "Features", raw
    int logicalCpus = 0;
    QVector<ArmCoreId> armCores;  // ARM: one per processor block
    QString hardware;             // ARM "Hardware" (older kernels)
    bool isArm() const { return !armCores.isEmpty(); }
};
CpuInfoText parseCpuInfo(const QString &text);

// MIDR_EL1 ("0x00000000410fd0b1") -> implementer/variant/part/revision.
std::optional<ArmCoreId> parseMidr(const QString &text);

// ARM implementer and core names from the MIDR fields. Unknown parts give
// "implementer 0x.. part 0x...".
QString armVendorName(int implementer);
QString armCoreName(int implementer, int part);

// Normalised ISA feature names, in a fixed order:
//   x86: sse2 sse3 ssse3 sse4.1 sse4.2 popcnt aes f16c fma bmi1 bmi2 avx avx2
//        sha avx512f avx512dq avx512cd avx512bw avx512vl avx512vnni avx512bf16
//        avxvnni amx
//   arm: neon fp16 dotprod i8mm bf16 sve sve2 sme crc32 aes sha2 lse
QStringList normalizeX86Flags(const QStringList &linuxFlags);
QStringList normalizeArmFeatures(const QStringList &linuxFeatures);
const QStringList &x86FeatureOrder();
const QStringList &armFeatureOrder();

// Sysfs CPU list "0-3,8,10-11" -> {0,1,2,3,8,10,11}.
QVector<int> parseCpuList(const QString &text);

// Sysfs cache size "32K", "1024K", "16M" -> bytes.
qint64 parseCacheSize(const QString &text);

// Strips padding and the placeholder strings firmware uses for "unknown"
// ("To Be Filled By O.E.M.", "Not Specified", "Default string", …).
QString cleanFirmwareString(const QString &s);

// GPU/NPU vendor names by PCI vendor id.
QString pciVendorName(quint32 vendorId);

} // namespace sb::sysinfo
