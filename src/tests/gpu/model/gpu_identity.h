// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GPU identity, the default pick and the OpenGL helpers (Phase 4a design
// §4.1-§4.3, P11-P13). Pure helpers over plain structs.

#include <QMap>
#include <QString>
#include <QVector>

#include <array>
#include <optional>

namespace sb::gpu {

enum class GpuKind { Discrete, Integrated, Software, Other };

struct ApiAvailability {
    bool ok = false;
    QString version, driver, note, reason;
    QString hint;  // with a reason: where to get what is missing (a URL), else empty
    bool operator==(const ApiAvailability &) const = default;
};

struct GpuEntry {
    QString id, name;
    quint32 vendorId = 0, deviceId = 0;
    GpuKind kind = GpuKind::Other;
    qint64 vramBytes = 0;
    QString pciSlot;                       // Linux "0000:01:00.0"
    QString kernelDriver;                  // Linux "nvidia", "amdgpu"
    std::optional<bool> drivesDisplay;     // a display output belongs to this GPU. Linux: a connected, enabled DRM connector (unknown: no DRM card); Windows: DXGI EnumOutputs
    QString luid;                          // Windows: the adapter's LUID, 16 hex digits (bytes in memory order); the run selects by it
    QMap<QString, ApiAvailability> apis;   // key: "vulkan","dx12","dx11","opengl","d2d"
    bool operator==(const GpuEntry &) const = default;
};

QString makeLinuxGpuId(quint32 vendor, quint32 device, const QString &pciSlot);  // "10de:2c05@0000:01:00.0"; empty slot -> "10de:2c05#0"
QString makeWindowsGpuId(quint32 vendor, quint32 device, int occurrence);        // "1002:73df#0"
// §4.1: among the non-software entries (all of them with allowSoftware), the
// discrete one with the most VRAM (ties: list order); else the first
// integrated; else the first entry; else none.
std::optional<int> defaultGpuIndex(const QVector<GpuEntry> &gpus, bool allowSoftware = false);
int findGpu(const QVector<GpuEntry> &gpus, const QString &id);  // index or -1
bool isGlOn12(const QString &glVendor, const QString &glRenderer);
QString glOn12DeviceName(const QString &glRenderer);  // "D3D12 (X)" -> "X"
bool isSoftwareGl(const QString &glRenderer);          // llvmpipe | softpipe | "GDI Generic" | "Microsoft Basic Render"
std::optional<quint32> glVendorToPci(const QString &glVendor);  // §4.3 table; GLon12 and unknown -> nullopt
// The LUID as 16 lowercase hex digits (8 bytes in memory order) and back.
QString luidHex(const quint8 *bytes);
bool parseLuidHex(const QString &text, std::array<quint8, 8> *out);
QString kindText(GpuKind);       // "discrete" "integrated" "software" "other"
QString vramText(qint64 bytes);  // round(bytes / 2^30) + " GB"; < 1 GiB: round(bytes / 2^20) + " MB"

} // namespace sb::gpu
