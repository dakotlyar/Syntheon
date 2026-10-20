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
    // Phase 3 P16: total and data width in bits (0x08, 0x0A), form factor
    // (0x0E: "DIMM", "SODIMM", "Row Of Chips", ...), rank (attributes 0x1B
    // bits 0-3, SMBIOS 2.6+). 0 / empty = unknown.
    int totalWidth = 0, dataWidth = 0;
    QString formFactor;
    int rank = 0;
    // The module's JEDEC manufacturer ID (type 17 0x2C, SMBIOS 3.2+; udev's
    // MODULE_MANUFACTURER_ID): bank 1-based, code as reported (parity bit
    // included). 0 = not reported. When the Manufacturer string is a
    // placeholder, `manufacturer` is decoded from it (jep106.h) and
    // manufacturerSource says which: "reported", "jedec", or empty.
    int moduleManufacturerBank = 0, moduleManufacturerCode = 0;
    QString manufacturerSource;
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
// SMBIOS type 17 form factor names, as systemd's dmi_memory_id spells them
// (so the udev fallback and the tables agree); empty for Other / Unknown.
QString smbiosFormFactorName(int formFactor);

// The Linux fallback when the helper isn't available (Phase 3 D11): the
// type-17 fields systemd-udev keeps in its world-readable
// /run/udev/data/+dmi:id ("E:MEMORY_DEVICE_<n>_<FIELD>=value"), for the
// populated devices, in device order. Only a whitelist of fields is read
// (size, type, speed, configured speed, rank, data/total width, form
// factor, locator, bank locator, part number, manufacturer): never
// SERIAL_NUMBER or ASSET_TAG (P23).
QVector<SmbiosMemoryDevice> parseUdevMemoryDevices(const QString &text);

} // namespace sb::sysinfo
