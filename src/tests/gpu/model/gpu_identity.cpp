// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/model/gpu_identity.h"

#include <cmath>

namespace sb::gpu {

QString makeLinuxGpuId(quint32 vendor, quint32 device, const QString &pciSlot)
{
    const QString head = QStringLiteral("%1:%2").arg(vendor, 4, 16, QLatin1Char('0')).arg(device, 4, 16, QLatin1Char('0'));
    return pciSlot.isEmpty() ? head + QStringLiteral("#0") : head + QLatin1Char('@') + pciSlot;
}

QString makeWindowsGpuId(quint32 vendor, quint32 device, int occurrence)
{
    return QStringLiteral("%1:%2#%3").arg(vendor, 4, 16, QLatin1Char('0')).arg(device, 4, 16, QLatin1Char('0')).arg(occurrence);
}

std::optional<int> defaultGpuIndex(const QVector<GpuEntry> &gpus, bool allowSoftware)
{
    int firstUsable = -1, firstIntegrated = -1, bestDiscrete = -1;
    for (int i = 0; i < gpus.size(); ++i) {
        const GpuEntry &g = gpus[i];
        if (g.kind == GpuKind::Software && !allowSoftware)
            continue;
        if (firstUsable < 0)
            firstUsable = i;
        if (g.kind == GpuKind::Integrated && firstIntegrated < 0)
            firstIntegrated = i;
        if (g.kind == GpuKind::Discrete && (bestDiscrete < 0 || g.vramBytes > gpus[bestDiscrete].vramBytes))
            bestDiscrete = i;
    }
    for (const int i : {bestDiscrete, firstIntegrated, firstUsable}) {
        if (i >= 0)
            return i;
    }
    return std::nullopt;
}

int findGpu(const QVector<GpuEntry> &gpus, const QString &id)
{
    for (int i = 0; i < gpus.size(); ++i) {
        if (gpus[i].id == id)
            return i;
    }
    return -1;
}

bool isGlOn12(const QString &glVendor, const QString &glRenderer)
{
    return glVendor == QLatin1StringView("Microsoft Corporation") && glRenderer.startsWith(QLatin1StringView("D3D12 ("));
}

QString glOn12DeviceName(const QString &glRenderer)
{
    const QLatin1StringView prefix("D3D12 (");
    if (!glRenderer.startsWith(prefix))
        return glRenderer;
    QString name = glRenderer.mid(prefix.size());
    if (name.endsWith(QLatin1Char(')')))
        name.chop(1);
    return name;
}

bool isSoftwareGl(const QString &glRenderer)
{
    for (const char *needle : {"llvmpipe", "softpipe", "GDI Generic", "Microsoft Basic Render"}) {
        if (glRenderer.contains(QLatin1StringView(needle), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

std::optional<quint32> glVendorToPci(const QString &glVendor)
{
    const auto has = [&](const char *s) { return glVendor.contains(QLatin1StringView(s), Qt::CaseSensitive); };
    if (has("NVIDIA"))
        return 0x10de;
    if (has("AMD") || has("ATI Technologies") || has("Advanced Micro Devices"))
        return 0x1002;
    if (has("Intel"))
        return 0x8086;
    if (has("Qualcomm"))
        return 0x5143;
    return std::nullopt;
}

QString kindText(GpuKind k)
{
    switch (k) {
    case GpuKind::Discrete: return QStringLiteral("discrete");
    case GpuKind::Integrated: return QStringLiteral("integrated");
    case GpuKind::Software: return QStringLiteral("software");
    case GpuKind::Other: break;
    }
    return QStringLiteral("other");
}

QString vramText(qint64 bytes)
{
    constexpr qint64 GiB = qint64(1) << 30, MiB = qint64(1) << 20;
    if (bytes >= GiB)
        return QStringLiteral("%1 GB").arg(std::llround(double(bytes) / double(GiB)));
    return QStringLiteral("%1 MB").arg(std::llround(double(bytes) / double(MiB)));
}

} // namespace sb::gpu
