// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Linux GPU list (Phase 4a design §4.1, §4.2, task 13): the Vulkan
// devices merged with the kernel's DRM cards. Pure; reads only the files under
// a root folder (tests point it at a fixture).

#include "tests/gpu/model/gpu_identity.h"

#include <QString>
#include <QVector>

#include <optional>

namespace sb::gpu {

struct VkDeviceRecord {
    QString name;
    quint32 vendorId = 0, deviceId = 0;
    QString type;  // "discrete", "integrated", "virtual", "cpu", "other"
    QString apiVersion, driverName, driverInfo;
    qint64 deviceLocalBytes = 0;
    QString pciSlot;
    quint8 luid[8] = {};  // Windows: VkPhysicalDeviceIDProperties::deviceLUID, when luidValid
    bool luidValid = false;
};
struct DrmCard {
    QString pciSlot;
    quint32 vendorId = 0, deviceId = 0;
    QString driver;
    std::optional<bool> drivesDisplay;  // a connector card<N>-* is connected and enabled (drm_display.h)
    bool operator==(const DrmCard &) const = default;
};

// The Linux `gpu.driver` text: the kernel driver and the Vulkan driver's own version ("amdgpu · Mesa 25.2.4",
// "nvidia · 615.78.08"); either part alone when the other is unknown.
QString linuxDriverText(const QString &kernelDriver, const QString &vulkanDriverInfo);

// apis["vulkan"] of a device: ok = apiVersion >= 1.3, else "Needs Vulkan 1.3 (the driver has 1.2.x)".
ApiAvailability vulkanAvailability(const VkDeviceRecord &d);

// <root>/sys/class/drm/card[0-9]+/device/uevent (DRIVER, PCI_ID, PCI_SLOT_NAME), in card order.
QVector<DrmCard> readDrmCards(const QString &root = QStringLiteral("/"));

// One entry per DRM card (its Vulkan device matched by PCI slot, else by
// vendor and device); Vulkan devices without a card are added (a "cpu" type,
// such as lavapipe, is kind Software). apis["vulkan"].ok = apiVersion >= 1.3;
// a card without a Vulkan device has the reason "This GPU has no Vulkan
// driver"; dx12, dx11 and d2d are "... is Windows only". OpenGL is the
// client's to fill (the GL probe).
QVector<GpuEntry> mergeLinuxGpus(const QVector<VkDeviceRecord> &vk, const QVector<DrmCard> &drm);

} // namespace sb::gpu
