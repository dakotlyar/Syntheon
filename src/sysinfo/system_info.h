// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/result.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sb {

// What System info shows and every run freezes into its SystemSnapshot
// (sysinfo_version 1). P23: never serial numbers, UUIDs or MAC addresses.
// Empty strings / zero / -1 mean "not known".

struct MachineInfo {
    QString maker, model, family;
    QString biosVendor, biosVersion, biosDate;
    QString hostname;
};

struct CacheSizes {  // total bytes across the CPU
    qint64 l1d = 0, l1i = 0, l2 = 0, l3 = 0;
};

struct CpuInfo {
    QString name;    // "AMD RYZEN AI MAX+ 392 w/ Radeon 8060S", "ARM Cortex-A76"
    QString vendor;  // "AMD", "Intel", "Qualcomm", "ARM"
    QString arch;    // "x86_64", "arm64"
    int packages = 0, cores = 0, threads = 0;
    // Hybrid parts: Intel P/E cores, ARM big/LITTLE (biggest tier = performance).
    int performanceCores = 0, efficiencyCores = 0;
    QStringList coreTypes;  // ARM: distinct core designs, e.g. {"Cortex-X4", "Cortex-A720"}
    CacheSizes caches;
    int baseMhz = 0, maxMhz = 0;
    QStringList features;  // normalised ISA features, see parsers/text_parsers.h
};

struct MemoryModule {
    qint64 sizeBytes = 0;
    QString type;  // "DDR5", "LPDDR5X"
    int speedMts = 0, configuredSpeedMts = 0;
    QString manufacturer, partNumber;
    QString locator, bankLocator;
};

struct MemoryInfo {
    qint64 totalBytes = 0;
    bool modulesKnown = false;  // false until SMBIOS has been read (Linux: helper)
    QVector<MemoryModule> modules;
    int channels = 0;  // estimate; 0 = unknown
};

struct GpuInfo {
    QString name, vendor;
    quint32 vendorId = 0, deviceId = 0;
    QString driver;         // kernel driver (Linux) or adapter driver
    QString driverVersion;  // "Mesa 25.2.0", "32.0.15.6094"
    qint64 vramBytes = 0;
    QString vulkanVersion;  // the device's Vulkan API version, if any
};

struct ApiInfo {
    QString vulkanInstanceVersion;  // empty = no Vulkan loader
    QString openGlVersion, openGlRenderer, openGlVendor;
    bool openGlIsEs = false;
    QString d3d12FeatureLevel, d3d11FeatureLevel;  // Windows
};

struct VolumeInfo {
    QString mount, fileSystem;
    qint64 totalBytes = 0, freeBytes = 0;
    QString model;  // the drive behind it
    QString bus;    // "NVMe", "SATA", "USB", …
    QString kind;   // "SSD", "HDD", ""
};

struct OsInfo {
    QString name, version, build, kernel, arch, session;
};

struct NpuInfo {
    QString name, vendor, driver, driverVersion;
    quint32 vendorId = 0, deviceId = 0;
};

struct BatteryInfo {
    bool present = false;
    double designWh = 0, fullWh = 0, healthPct = 0, chargePct = -1;
    bool acOnline = false;
    int cycleCount = -1;
};

struct AppInfo {
    QString version, build, qtVersion, compiler, gitSha;
};

struct SystemInfo {
    MachineInfo machine;
    CpuInfo cpu;
    MemoryInfo memory;
    QVector<GpuInfo> gpus;
    ApiInfo apis;
    QVector<VolumeInfo> volumes;
    OsInfo os;
    QVector<NpuInfo> npus;
    BatteryInfo battery;
    AppInfo app;
    bool emulated = false;  // D9: x64 build running emulated on ARM64 Windows
    QString nativeArch;     // the machine's own architecture when emulated

    // History's "Machine" column and the header strip: "Knowhere · ASUS TUF
    // Gaming A14" (host name + maker and family/model).
    QString machineLabel() const;
    QJsonObject toJson() const;
    SystemSnapshot toSnapshot() const;
};

} // namespace sb
