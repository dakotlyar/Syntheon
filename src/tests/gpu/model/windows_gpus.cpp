// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/model/windows_gpus.h"

#include <QMap>

#include <cstring>

namespace sb::gpu {

QVector<GpuEntry> mergeWindowsGpus(const QVector<DxgiAdapterRecord> &adapters, const QVector<VkDeviceRecord> &vk)
{
    QVector<GpuEntry> out;
    QMap<quint64, int> seen;  // vendor << 32 | device -> how many so far
    for (const DxgiAdapterRecord &a : adapters) {
        GpuEntry g;
        g.vendorId = a.vendorId;
        g.deviceId = a.deviceId;
        g.name = a.name;
        g.vramBytes = a.dedicatedBytes;
        // Software first: WARP reports UMA, which must not make it "integrated". The adapter flag, or Microsoft's
        // Basic Render Driver (1414:008c) when a list was built without it.
        const bool software = a.software || (a.vendorId == 0x1414 && a.deviceId == 0x008c);
        const bool amdOrIntel = a.vendorId == 0x1002 || a.vendorId == 0x8086;
        const bool integrated = a.umaKnown ? a.uma : (amdOrIntel && a.sharedBytes > a.dedicatedBytes);
        g.kind = software ? GpuKind::Software
                 : integrated || a.dedicatedBytes < (qint64(512) << 20) ? GpuKind::Integrated
                                                                        : GpuKind::Discrete;
        g.luid = luidHex(a.luid);
        const int n = seen[(quint64(a.vendorId) << 32) | a.deviceId]++;
        g.id = makeWindowsGpuId(a.vendorId, a.deviceId, n);
        ApiAvailability none;
        none.reason = QStringLiteral("This GPU has no Vulkan driver");
        g.apis.insert(QStringLiteral("vulkan"), none);
        for (const VkDeviceRecord &d : vk) {
            if (d.luidValid && std::memcmp(d.luid, a.luid, sizeof a.luid) == 0) {
                g.apis[QStringLiteral("vulkan")] = vulkanAvailability(d);
                break;
            }
        }
        g.apis.insert(QStringLiteral("dx12"), a.dx12);
        g.apis.insert(QStringLiteral("dx11"), a.dx11);
        g.apis.insert(QStringLiteral("d2d"), a.d2d);
        out.append(g);
    }
    return out;
}

} // namespace sb::gpu
