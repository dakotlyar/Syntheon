// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/vulkan/vk_probe.h"

#include "tests/gpu/worker/vulkan/vk_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sb::gpu {

namespace {

std::string versionString(std::uint32_t v)
{
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." + std::to_string(VK_API_VERSION_PATCH(v));
}

const char *typeName(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

} // namespace

std::vector<VkDeviceRecordStd> probeVulkanDevices(std::string *error)
{
    std::vector<VkDeviceRecordStd> out;
    if (std::string e = vk::loadVulkanLoader(); !e.empty()) {
        *error = e;
        return out;
    }
    std::uint32_t instanceVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&instanceVersion);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SynBench";
    app.pEngineName = "SynBench";
    app.apiVersion = std::min<std::uint32_t>(instanceVersion, VK_API_VERSION_1_3);
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateInstance(&ci, nullptr, &instance); r != VK_SUCCESS) {
        *error = vk::failure("vkCreateInstance", r);
        return out;
    }
    volkLoadInstance(instance);
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (count && vkEnumeratePhysicalDevices(instance, &count, devices.data()) >= VK_SUCCESS) {
        devices.resize(count);
        for (VkPhysicalDevice pd : devices) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(pd, &p);
            VkDeviceRecordStd d;
            d.name = p.deviceName;
            d.vendorId = p.vendorID;
            d.deviceId = p.deviceID;
            d.type = typeName(p.deviceType);
            d.apiVersion = versionString(p.apiVersion);

            std::uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> exts(extCount);
            vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data());
            const bool hasPci = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties &e) {
                return std::strcmp(e.extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME) == 0;
            });
            VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDevicePCIBusInfoPropertiesEXT pci{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            void **tail = &p2.pNext;
            if (p.apiVersion >= VK_API_VERSION_1_2) {
                *tail = &driver;
                tail = &driver.pNext;
            }
            if (hasPci)
                *tail = &pci;
            if (instanceVersion >= VK_API_VERSION_1_1 && p.apiVersion >= VK_API_VERSION_1_1) {
                vkGetPhysicalDeviceProperties2(pd, &p2);
                if (p.apiVersion >= VK_API_VERSION_1_2) {
                    d.driverName = driver.driverName;
                    d.driverInfo = driver.driverInfo;
                }
                if (hasPci) {
                    char slot[32];
                    std::snprintf(slot, sizeof slot, "%04x:%02x:%02x.%x", pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction);
                    d.pciSlot = slot;
                }
            }
            VkPhysicalDeviceMemoryProperties mem{};
            vkGetPhysicalDeviceMemoryProperties(pd, &mem);
            for (std::uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
                if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    d.deviceLocalBytes = std::max<std::int64_t>(d.deviceLocalBytes, std::int64_t(mem.memoryHeaps[i].size));
            }
            out.push_back(std::move(d));
        }
    }
    vkDestroyInstance(instance, nullptr);
    return out;
}

} // namespace sb::gpu
