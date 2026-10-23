// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/model/linux_gpus.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include "tests/gpu/model/drm_display.h"

#include <algorithm>

namespace sb::gpu {

namespace {

GpuKind kindOf(const QString &type)
{
    if (type == QLatin1StringView("discrete"))
        return GpuKind::Discrete;
    if (type == QLatin1StringView("integrated"))
        return GpuKind::Integrated;
    if (type == QLatin1StringView("cpu"))
        return GpuKind::Software;
    return GpuKind::Other;
}

// "1.2.198" -> (1, 2).
std::pair<int, int> majorMinor(const QString &v)
{
    const QStringList p = v.split(QLatin1Char('.'));
    return {p.value(0).toInt(), p.value(1).toInt()};
}

void addWindowsOnly(GpuEntry &g)
{
    ApiAvailability dx;
    dx.reason = QStringLiteral("DirectX is Windows only");
    g.apis.insert(QStringLiteral("dx12"), dx);
    g.apis.insert(QStringLiteral("dx11"), dx);
    ApiAvailability d2d;
    d2d.reason = QStringLiteral("Direct2D is Windows only");
    g.apis.insert(QStringLiteral("d2d"), d2d);
}

} // namespace

ApiAvailability vulkanAvailability(const VkDeviceRecord &d)
{
    ApiAvailability a;
    const auto [major, minor] = majorMinor(d.apiVersion);
    if (major > 1 || (major == 1 && minor >= 3)) {
        a.ok = true;
        a.version = QStringLiteral("Vulkan ") + d.apiVersion;
        a.driver = (d.driverName + QLatin1Char(' ') + d.driverInfo).trimmed();
    } else {
        a.reason = QStringLiteral("Needs Vulkan 1.3 (the driver has %1)").arg(d.apiVersion);
    }
    return a;
}

QVector<DrmCard> readDrmCards(const QString &root)
{
    QVector<std::pair<int, DrmCard>> found;
    const QDir drm(QDir(root).filePath(QStringLiteral("sys/class/drm")));
    static const QRegularExpression cardName(QStringLiteral("^card(\\d+)$"));
    for (const QString &name : drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QRegularExpressionMatch m = cardName.match(name);
        if (!m.hasMatch())
            continue;
        QFile f(drm.filePath(name + QStringLiteral("/device/uevent")));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        DrmCard c;
        for (const QByteArray &raw : f.readAll().split('\n')) {
            const QString line = QString::fromUtf8(raw).trimmed();
            const qsizetype eq = line.indexOf(QLatin1Char('='));
            if (eq < 0)
                continue;
            const QString key = line.left(eq), value = line.mid(eq + 1);
            if (key == QLatin1StringView("DRIVER")) {
                c.driver = value;
            } else if (key == QLatin1StringView("PCI_SLOT_NAME")) {
                c.pciSlot = value;
            } else if (key == QLatin1StringView("PCI_ID")) {
                const QStringList ids = value.split(QLatin1Char(':'));
                c.vendorId = ids.value(0).toUInt(nullptr, 16);
                c.deviceId = ids.value(1).toUInt(nullptr, 16);
            }
        }
        if (c.vendorId == 0 && c.deviceId == 0)
            continue;  // not a PCI device
        c.drivesDisplay = drmCardDrivesDisplay(drm.path().toStdString(), m.captured(1).toInt());
        found.append({m.captured(1).toInt(), c});
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    QVector<DrmCard> out;
    for (const auto &p : std::as_const(found))
        out.append(p.second);
    return out;
}

QString linuxDriverText(const QString &kernelDriver, const QString &vulkanDriverInfo)
{
    QString info = vulkanDriverInfo.trimmed();
    if (const qsizetype paren = info.indexOf(QStringLiteral(" (")); paren > 0)  // "(LLVM 20.1.8)"
        info.truncate(paren);
    if (kernelDriver.isEmpty() || info.isEmpty())
        return kernelDriver + info;
    return kernelDriver + QStringLiteral(" \u00b7 ") + info;
}

QVector<GpuEntry> mergeLinuxGpus(const QVector<VkDeviceRecord> &vk, const QVector<DrmCard> &drm)
{
    QVector<GpuEntry> out;
    QVector<bool> used(vk.size(), false);
    const auto matchVulkan = [&](const DrmCard &c) -> int {
        for (int i = 0; i < vk.size(); ++i) {
            if (!used[i] && !c.pciSlot.isEmpty() && vk[i].pciSlot == c.pciSlot)
                return i;
        }
        for (int i = 0; i < vk.size(); ++i) {
            if (!used[i] && vk[i].vendorId == c.vendorId && vk[i].deviceId == c.deviceId)
                return i;
        }
        return -1;
    };
    for (const DrmCard &c : drm) {
        GpuEntry g;
        g.vendorId = c.vendorId;
        g.deviceId = c.deviceId;
        g.pciSlot = c.pciSlot;
        g.kernelDriver = c.driver;
        g.drivesDisplay = c.drivesDisplay;
        g.id = makeLinuxGpuId(c.vendorId, c.deviceId, c.pciSlot);
        const int v = matchVulkan(c);
        if (v >= 0) {
            used[v] = true;
            g.name = vk[v].name;
            g.kind = kindOf(vk[v].type);
            g.vramBytes = vk[v].deviceLocalBytes;
            g.driverVersion = linuxDriverText(c.driver, vk[v].driverInfo);
            g.apis.insert(QStringLiteral("vulkan"), vulkanAvailability(vk[v]));
        } else {
            g.name = QStringLiteral("GPU %1:%2").arg(c.vendorId, 4, 16, QLatin1Char('0')).arg(c.deviceId, 4, 16, QLatin1Char('0'));
            ApiAvailability none;
            none.reason = QStringLiteral("This GPU has no Vulkan driver");
            g.apis.insert(QStringLiteral("vulkan"), none);
        }
        if (g.driverVersion.isEmpty())
            g.driverVersion = c.driver;
        addWindowsOnly(g);
        out.append(g);
    }
    // Vulkan devices without a card (lavapipe, or a driver that hides its bus).
    QMap<QString, int> seen;  // "vendor:device" -> how many added without a slot
    for (int i = 0; i < vk.size(); ++i) {
        if (used[i])
            continue;
        const VkDeviceRecord &d = vk[i];
        GpuEntry g;
        g.vendorId = d.vendorId;
        g.deviceId = d.deviceId;
        g.pciSlot = d.pciSlot;
        g.name = d.name;
        g.kind = kindOf(d.type);
        g.vramBytes = d.deviceLocalBytes;
        g.driverVersion = linuxDriverText({}, d.driverInfo);  // no kernel driver to name: the Vulkan driver's version alone (AP-5 L1)
        g.id = makeLinuxGpuId(d.vendorId, d.deviceId, d.pciSlot);
        if (d.pciSlot.isEmpty()) {
            const QString key = QStringLiteral("%1:%2").arg(d.vendorId).arg(d.deviceId);
            const int n = seen[key]++;
            g.id = g.id.left(g.id.lastIndexOf(QLatin1Char('#'))) + QLatin1Char('#') + QString::number(n);  // P11: the n-th such device
        }
        g.apis.insert(QStringLiteral("vulkan"), vulkanAvailability(d));
        addWindowsOnly(g);
        out.append(g);
    }
    return out;
}

} // namespace sb::gpu
