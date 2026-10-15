// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

namespace sb::sysinfo {

// SMBIOS (DMTF DSP0134) parsing, done in the unprivileged process: on Linux
// the root helper only hands over the raw bytes (READ_SMBIOS, task 10); on
// Windows they come from GetSystemFirmwareTable('RSMB'). Every read is
// bounds-checked; malformed input yields fewer fields, never a crash.
//
// P23: serial numbers, asset tags and the system UUID are never read.

struct SmbiosVersion {
    int major = 0, minor = 0;
};

struct SmbiosMemoryDevice {
    qint64 sizeBytes = 0;  // 0 = empty slot
    QString type;          // "DDR5", "LPDDR5", …
    int speedMts = 0;
    int configuredSpeedMts = 0;
    QString manufacturer, partNumber;
    QString locator, bankLocator;
};

struct SmbiosInfo {
    SmbiosVersion version;
    QString biosVendor, biosVersion, biosDate;      // type 0
    QString maker, product, productVersion, family; // type 1
    QVector<SmbiosMemoryDevice> memoryDevices;      // type 17, populated slots only
};

// Linux /sys/firmware/dmi/tables/smbios_entry_point ("_SM_" or "_SM3_").
std::optional<SmbiosVersion> parseSmbiosEntryPoint(const QByteArray &entryPoint);

// The structure table (/sys/firmware/dmi/tables/DMI).
SmbiosInfo parseSmbiosTable(const QByteArray &table, SmbiosVersion version);

// Windows RawSMBIOSData: an 8-byte header (calling method, major, minor, DMI
// revision, u32 length) followed by the table.
std::optional<SmbiosInfo> parseWindowsRawSmbios(const QByteArray &raw);

// Memory channel estimate from the populated modules' locators ("CHANNEL A",
// "DIMM_A1", "P0 CHANNEL B" …); falls back to the module count, capped at 2,
// when locators don't say. 0 when there are no modules.
int estimateMemoryChannels(const QVector<SmbiosMemoryDevice> &modules);

QString smbiosMemoryTypeName(int type);

} // namespace sb::sysinfo
