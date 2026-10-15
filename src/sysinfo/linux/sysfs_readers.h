// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sysinfo/system_info.h"

#include <QString>

namespace sb::sysinfo::linux_sysfs {

// Readers over a Linux file tree. `root` is "/" on a live system, or a
// fixture tree captured with tests/fixtures/capture-linux.sh.

// /sys/class/dmi/id (no serials, no UUID); ARM fallback /proc/device-tree/model.
MachineInfo readMachine(const QString &root);

// Topology, caches, clocks and ARM core types, from /sys/devices/system/cpu
// (and /proc/cpuinfo for the name, vendor and features). x86 features here
// come from cpuinfo flags; the live collector prefers CPUID.
CpuInfo readCpu(const QString &root);

qint64 readMemTotal(const QString &root);
OsInfo readOsRelease(const QString &root);

// /sys/class/drm/cardN: PCI ids, kernel driver, VRAM (amdgpu).
QVector<GpuInfo> readGpus(const QString &root);
// /sys/class/accel/accelN: intel_vpu / amdxdna (presence only).
QVector<NpuInfo> readNpus(const QString &root);
// /sys/class/power_supply, skipping peripherals (scope=Device: mice, pads).
BatteryInfo readBattery(const QString &root);

// /sys/dev/block/<major>:<minor> -> the whole disk's model, bus and SSD/HDD.
struct BlockDeviceInfo {
    QString model, bus, kind;
};
BlockDeviceInfo readBlockDevice(const QString &root, unsigned major, unsigned minor);

} // namespace sb::sysinfo::linux_sysfs
