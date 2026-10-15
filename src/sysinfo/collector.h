// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

AppInfo appInfo();

} // namespace sb::sysinfo
