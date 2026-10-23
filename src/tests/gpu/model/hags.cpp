// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/model/hags.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sb::gpu {

std::string hagsText(std::optional<unsigned long> hwSchMode)
{
    if (!hwSchMode)
        return {};
    return *hwSchMode == 2 ? "on" : *hwSchMode == 1 ? "off" : std::string();
}

std::optional<unsigned long> readHwSchMode()
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
