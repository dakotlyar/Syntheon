// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sysinfo/system_info.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

namespace sb::sysinfo::win {

// Pure helpers for the Windows collector, kept portable so they're unit-tested
// on every platform. No Windows headers here.

// "PCI\VEN_8086&DEV_7D1D&SUBSYS_…" -> (0x8086, 0x7d1d).
std::optional<std::pair<quint32, quint32>> parseHardwareId(const QString &hardwareId);

// STORAGE_BUS_TYPE -> "NVMe", "SATA", "USB", …
QString busTypeName(int busType);

// STORAGE_DEVICE_DESCRIPTOR (IOCTL_STORAGE_QUERY_PROPERTY,
// StorageDeviceProperty): vendor + product strings and the bus type. The
// serial number offset is deliberately never followed (P23).
struct StorageDescriptor {
    QString vendor, product;
    int busType = 0;
    QString model() const;
};
std::optional<StorageDescriptor> parseStorageDeviceDescriptor(const QByteArray &buffer);

// "Windows 10 Pro" + build 26100 -> "Windows 11 Pro" (the registry's
// ProductName still says 10 on Windows 11).
QString windowsName(const QString &productName, int build);

// UMD driver version from IDXGIAdapter::CheckInterfaceSupport -> "32.0.15.6094".
QString dxgiDriverVersion(qint64 umdVersion);

// D3D_FEATURE_LEVEL (0xc200) -> "12_2".
QString featureLevelName(int level);

// D9: an x64 build is emulated when IsWow64Process2 reports an ARM64 native
// machine (IMAGE_FILE_MACHINE_ARM64 = 0xAA64). Returns the native arch name
// when emulated, else empty.
QString emulatedNativeArch(bool buildIsX64, int nativeMachine);

// One BATTERY_INFORMATION. Capacities are mWh unless relative.
struct BatteryFields {
    bool relative = false;  // BATTERY_CAPACITY_RELATIVE: units aren't mWh
    quint32 designedCapacity = 0, fullChargedCapacity = 0, cycleCount = 0;
};
// Combines the batteries and GetSystemPowerStatus (ACLineStatus,
// BatteryLifePercent 0-100 or 255, BatteryFlag 128 = no battery).
BatteryInfo combineBattery(const QVector<BatteryFields> &batteries, int acLineStatus, int lifePercent,
                           int batteryFlag);

// GetLogicalProcessorInformationEx, reduced to what's counted.
struct CoreEntry {
    int efficiencyClass = 0;  // higher = faster (Windows 10+ hybrid)
    int logicalProcessors = 1;
};
struct CacheEntry {
    int level = 0;
    int type = 0;  // PROCESSOR_CACHE_TYPE: 0 unified, 1 instruction, 2 data, 3 trace
    qint64 size = 0;
};
void countCores(const QVector<CoreEntry> &cores, CpuInfo &cpu);
CacheSizes sumCaches(const QVector<CacheEntry> &caches);

} // namespace sb::sysinfo::win
