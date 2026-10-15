// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QVector>

namespace sb::sysinfo {

struct VulkanDevice {
    QString name;
    quint32 vendorId = 0, deviceId = 0;
    QString type;        // "integrated", "discrete", "virtual", "cpu", "other"
    QString apiVersion;  // "1.4.318"
    QString driverName;  // "radv" (Vulkan 1.2+)
    QString driverInfo;  // "Mesa 25.2.0"
    qint64 deviceLocalBytes = 0;  // largest device-local heap
};

struct VulkanInfo {
    bool loaderFound = false;
    QString instanceVersion;  // empty if no instance could be created
    QVector<VulkanDevice> devices;
    QString error;
};

// Loads the Vulkan loader at run time (vulkan-1.dll / libvulkan.so.1, never
// linked, §2.1), creates a bare instance and lists the physical devices. Safe
// on any thread.
VulkanInfo probeVulkan();

} // namespace sb::sysinfo
