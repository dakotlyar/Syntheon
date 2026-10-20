// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Memory module makers by JEDEC JEP106 manufacturer ID (SMBIOS type 17's
// Module Manufacturer ID, SMBIOS 3.2+; udev's MODULE_MANUFACTURER_ID). Some
// firmware leaves the Manufacturer string as "Unknown" while the ID is
// right there (Tiamat: "Unknown", Bank 5 0xCD = G.Skill), so the ID names
// the maker when the string is a placeholder. A real string always wins.
//
// The table is a short, curated list of DRAM module and chip makers seen in
// desktops and laptops, every entry taken from (and checked against) the
// JEP106 table in i2c-tools' decode-dimms. An ID not in it reads
// "JEDEC bank N, 0xXX".

#include <QString>

namespace sb::sysinfo {

// The maker for JEP106 bank `bank` (1-based: continuation codes + 1) and
// manufacturer code `code` (the byte as reported, odd-parity bit included;
// the parity bit is masked off, as dmidecode does). Empty if not listed.
QString jep106Name(int bank, int code);

// A Manufacturer string that says nothing: empty, "Unknown", "Not
// Specified", "Undefined", "To Be Filled By O.E.M.", "Manufacturer0"-style,
// or a bare hex number ("CE00000000000000", "0x802C").
bool isPlaceholderManufacturer(const QString &s);

struct ModuleManufacturer {
    QString name;
    // "reported" (the Manufacturer string), "jedec" (decoded from the
    // module manufacturer ID), or empty (neither said anything).
    QString source;
};
// The module manufacturer to show: the reported string unless it is a
// placeholder and an ID is known (bank > 0), then the decoded name (or
// "JEDEC bank N, 0xXX"). A placeholder without an ID stays as reported.
ModuleManufacturer resolveModuleManufacturer(const QString &reported, int bank, int code);

} // namespace sb::sysinfo
