// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Does a GPU drive an enabled display output? (Phase 4a, Dave's Wayland OpenGL rule.) Reads
// <root>/sys/class/drm: a card drives a display when one of its connectors, /sys/class/drm/card<N>-*,
// has status "connected" AND enabled "enabled". Header-only and std only, so the Qt model and the Qt-free
// GL backend share it. A card is matched to a GPU by its PCI slot (device/uevent PCI_SLOT_NAME).

#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace sb::gpu {

namespace detail {
inline std::string drmFirstLine(const std::filesystem::path &file)
{
    std::ifstream f(file);
    std::string line;
    std::getline(f, line);
    return line;
}
} // namespace detail

// card<N> by its index: true when one of its connectors is connected and enabled.
inline bool drmCardDrivesDisplay(const std::filesystem::path &drmDir, int cardIndex)
{
    const std::string prefix = "card" + std::to_string(cardIndex) + "-";
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(drmDir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0)
            continue;
        if (detail::drmFirstLine(entry.path() / "status") == "connected" && detail::drmFirstLine(entry.path() / "enabled") == "enabled")
            return true;
    }
    return false;
}

// The GPU with this PCI slot ("0000:01:00.0"): nullopt when no card has it (unknown), else whether it drives a display.
inline std::optional<bool> drmGpuDrivesDisplay(const std::filesystem::path &root, const std::string &pciSlot)
{
    if (pciSlot.empty())
        return std::nullopt;
    const std::filesystem::path drm = root / "sys" / "class" / "drm";
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(drm, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() < 5 || name.rfind("card", 0) != 0 || name.find('-') != std::string::npos)
            continue;
        std::ifstream f(entry.path() / "device" / "uevent");
        for (std::string line; std::getline(f, line);) {
            if (line == "PCI_SLOT_NAME=" + pciSlot)
                return drmCardDrivesDisplay(drm, std::atoi(name.c_str() + 4));
        }
    }
    return std::nullopt;
}

} // namespace sb::gpu
