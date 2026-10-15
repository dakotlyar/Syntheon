// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/common/vulkan_probe.h"

#include <QLibrary>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>

namespace sb::sysinfo {

namespace {

QString versionString(uint32_t v)
{
    return QStringLiteral("%1.%2.%3").arg(VK_API_VERSION_MAJOR(v)).arg(VK_API_VERSION_MINOR(v)).arg(VK_API_VERSION_PATCH(v));
}

QString typeName(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return QStringLiteral("integrated");
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return QStringLiteral("discrete");
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return QStringLiteral("virtual");
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return QStringLiteral("cpu");
    default: return QStringLiteral("other");
    }
}

} // namespace

VulkanInfo probeVulkan()
{
    VulkanInfo info;
#if defined(Q_OS_WIN)
    QLibrary lib(QStringLiteral("vulkan-1"));
#else
    QLibrary lib(QStringLiteral("vulkan"), 1);
#endif
    if (!lib.load()) {
        info.error = QStringLiteral("no Vulkan loader");
        return info;
    }
    info.loaderFound = true;

    const auto getInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(lib.resolve("vkGetInstanceProcAddr"));
    if (!getInstanceProcAddr) {
        info.error = QStringLiteral("vkGetInstanceProcAddr missing");
        return info;
    }
    const auto global = [&](const char *name) { return getInstanceProcAddr(VK_NULL_HANDLE, name); };

    uint32_t instanceVersion = VK_API_VERSION_1_0;
    if (const auto enumerateVersion =
            reinterpret_cast<PFN_vkEnumerateInstanceVersion>(global("vkEnumerateInstanceVersion")))
        enumerateVersion(&instanceVersion);

    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(global("vkCreateInstance"));
    if (!createInstance) {
        info.error = QStringLiteral("vkCreateInstance missing");
        return info;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "SynBench";
    app.pEngineName = "SynBench";
    app.apiVersion = std::min<uint32_t>(instanceVersion, VK_API_VERSION_1_3);
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (createInstance(&ci, nullptr, &instance) != VK_SUCCESS) {
        info.error = QStringLiteral("vkCreateInstance failed");
        return info;
    }
    info.instanceVersion = versionString(instanceVersion);

    const auto fn = [&](const char *name) { return getInstanceProcAddr(instance, name); };
    const auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(fn("vkDestroyInstance"));
    const auto enumerateDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(fn("vkEnumeratePhysicalDevices"));
    const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(fn("vkGetPhysicalDeviceProperties"));
    const auto getProperties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(fn("vkGetPhysicalDeviceProperties2"));
    const auto getMemory =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(fn("vkGetPhysicalDeviceMemoryProperties"));

    if (enumerateDevices && getProperties && getMemory) {
        uint32_t count = 0;
        enumerateDevices(instance, &count, nullptr);
        QVector<VkPhysicalDevice> devices(count);
        if (count && enumerateDevices(instance, &count, devices.data()) >= VK_SUCCESS) {
            devices.resize(count);
            for (VkPhysicalDevice pd : std::as_const(devices)) {
                VkPhysicalDeviceProperties p{};
                getProperties(pd, &p);
                VulkanDevice d;
                d.name = QString::fromUtf8(p.deviceName);
                d.vendorId = p.vendorID;
                d.deviceId = p.deviceID;
                d.type = typeName(p.deviceType);
                d.apiVersion = versionString(p.apiVersion);
                if (getProperties2 && instanceVersion >= VK_API_VERSION_1_1 && p.apiVersion >= VK_API_VERSION_1_2) {
                    VkPhysicalDeviceDriverProperties driver{};
                    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
                    VkPhysicalDeviceProperties2 p2{};
                    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                    p2.pNext = &driver;
                    getProperties2(pd, &p2);
                    d.driverName = QString::fromUtf8(driver.driverName);
                    d.driverInfo = QString::fromUtf8(driver.driverInfo);
                }
                VkPhysicalDeviceMemoryProperties mem{};
                getMemory(pd, &mem);
                for (uint32_t i = 0; i < mem.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i) {
                    if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                        d.deviceLocalBytes = std::max<qint64>(d.deviceLocalBytes, qint64(mem.memoryHeaps[i].size));
                }
                info.devices.append(d);
            }
        }
    }
    if (destroyInstance)
        destroyInstance(instance, nullptr);
    return info;
}

} // namespace sb::sysinfo
