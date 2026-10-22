// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Windows GPU list (Phase 4a design §4.1, §4.2, task 23): the DXGI adapters (what the worker's
// `--probe` found, in DXGI high-performance order) with their Direct3D availability, and the Vulkan
// devices matched to them by LUID. Pure.

#include "tests/gpu/model/gpu_identity.h"
#include "tests/gpu/model/linux_gpus.h"

#include <QString>
#include <QVector>

namespace sb::gpu {

struct DxgiAdapterRecord {
    QString name;
    quint32 vendorId = 0, deviceId = 0;
    bool software = false;  // DXGI_ADAPTER_FLAG_SOFTWARE (only listed with --allow-software)
    qint64 dedicatedBytes = 0, sharedBytes = 0;  // DedicatedVideoMemory, SharedSystemMemory
    bool umaKnown = false, uma = false;          // D3D12_FEATURE_DATA_ARCHITECTURE::UMA (when a D3D12 device could be made)
    quint8 luid[8] = {};
    ApiAvailability dx12, dx11, d2d;
};

// One entry per adapter, in the order given. Id "%04x:%04x#n", n counting the adapters of that
// vendor:device in this list: the same numbering dxgi::findAdapter uses (software adapters are
// only in the list with --allow-software). Kind: software; else discrete when the dedicated
// memory is >= 512 MiB, else integrated; (checkpoint 3, L5) an adapter with UMA is integrated whatever its memory, and without the UMA answer an AMD or Intel adapter whose shared system memory exceeds its dedicated memory is too. The entry's luid is the adapter's (the run selects by it). A Vulkan device with the adapter's LUID is its Vulkan
// entry; without one: "This GPU has no Vulkan driver". OpenGL is the client's (the GL probe).
QVector<GpuEntry> mergeWindowsGpus(const QVector<DxgiAdapterRecord> &adapters, const QVector<VkDeviceRecord> &vk);

} // namespace sb::gpu
