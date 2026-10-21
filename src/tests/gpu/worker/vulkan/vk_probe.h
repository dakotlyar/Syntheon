// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Vulkan side of `synbench-gpu --probe` (Phase 4a design §4.1, task 13):
// every physical device the loader shows, with the facts the GPU list needs.
// Qt-free; main.cpp converts the records for model/linux_gpus.

#include <cstdint>
#include <string>
#include <vector>

namespace sb::gpu {

struct VkDeviceRecordStd {
    std::string name;
    std::uint32_t vendorId = 0, deviceId = 0;
    std::string type;  // "discrete", "integrated", "virtual", "cpu", "other"
    std::string apiVersion, driverName, driverInfo;  // "1.4.351"
    std::int64_t deviceLocalBytes = 0;
    std::string pciSlot;  // "0000:01:00.0" when VK_EXT_pci_bus_info is there, else ""
};

// Loads the loader (Windows: System32's only), makes a 1.3 instance (or the
// highest the loader has) and lists its devices. On failure the list is empty
// and *error says why ("N/A: No Vulkan driver is installed" for no loader).
std::vector<VkDeviceRecordStd> probeVulkanDevices(std::string *error);

} // namespace sb::gpu
