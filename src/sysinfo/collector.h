// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "privileged/privileged_ops.h"
#include "sysinfo/parsers/smbios.h"
#include "sysinfo/system_info.h"

namespace sb::sysinfo {

// The full snapshot except OpenGL (P13: native APIs, no WMI, no shelling
// out). Takes a few hundred ms (Vulkan instance creation dominates); call it
// on a pool thread at startup. Memory modules need SMBIOS: on Linux that's
// root-only, so they arrive later through applySmbios() (task 10).
SystemInfo collectSystemInfo();

// GUI thread only (needs a QGuiApplication): fills apis.openGl*.
void addOpenGlInfo(SystemInfo &info);

// Fills memory modules and the channel estimate (and any machine/BIOS fields
// still empty) from parsed SMBIOS.
void applySmbios(SystemInfo &info, const SmbiosInfo &smbios);

// Raw SMBIOS from PrivilegedOps (Linux: helper READ_SMBIOS, entry point +
// table; Windows: in-process, version + table) -> parsed. nullopt if the
// entry point is unrecognisable.
std::optional<SmbiosInfo> parseSmbiosTables(const SmbiosTables &tables);

// Reads SMBIOS through `ops` and applies it. On Linux this is what makes the
// memory modules appear once the helper is up (task 10).
bool applyPrivilegedSmbios(SystemInfo &info, PrivilegedOps &ops, PrivError *error = nullptr);

// The cheap, changing part of the snapshot: the power state polled every 5 s
// for the header ("on AC power") and the Battery preconditions.
BatteryInfo readBatteryNow();

AppInfo appInfo();

} // namespace sb::sysinfo
