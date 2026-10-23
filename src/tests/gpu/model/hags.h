// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Hardware-accelerated GPU scheduling (Phase 4a design §10, `gpu.hags`): Windows only, read only, from
// HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\HwSchMode (REG_DWORD: 2 = on, 1 = off). Std only: the
// registry read lives in hags.cpp, so no Windows header reaches the files that include this one (checkpoint 6, B1).

#include <optional>
#include <string>

namespace sb::gpu {

// "on" for 2, "off" for 1, empty for anything else (an unknown value stores nothing).
std::string hagsText(std::optional<unsigned long> hwSchMode);

// The value, or nullopt where it can't be read (not Windows, no such value on this build).
std::optional<unsigned long> readHwSchMode();

} // namespace sb::gpu
