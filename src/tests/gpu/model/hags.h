// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Hardware-accelerated GPU scheduling (Phase 4a design §10, `gpu.hags`): Windows only, read only, from
// HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\HwSchMode (REG_DWORD: 2 = on, 1 = off). Header-only
// and std only (plus <windows.h> on Windows), so the mapping tests everywhere.

#include <optional>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sb::gpu {

// "on" for 2, "off" for 1, empty for anything else (an unknown value stores nothing).
inline std::string hagsText(std::optional<unsigned long> hwSchMode)
{
    if (!hwSchMode)
        return {};
    return *hwSchMode == 2 ? "on" : *hwSchMode == 1 ? "off" : std::string();
}

// The value, or nullopt where it can't be read (not Windows, no such value on this build).
inline std::optional<unsigned long> readHwSchMode()
{
#if defined(_WIN32)
    DWORD value = 0, size = sizeof value;
    const LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode",
                                    RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (st == ERROR_SUCCESS)
        return static_cast<unsigned long>(value);
#endif
    return std::nullopt;
}

} // namespace sb::gpu
