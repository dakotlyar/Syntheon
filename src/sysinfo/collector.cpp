// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/collector.h"

#include "sysinfo/common/opengl_probe.h"

#include <QDir>
#include <QFile>

namespace sb::sysinfo {

void addOpenGlInfo(SystemInfo &info)
{
    const OpenGlInfo gl = probeOpenGl();
    if (!gl.ok)
        return;
    info.apis.openGlVersion = gl.version;
    info.apis.openGlRenderer = gl.renderer;
    info.apis.openGlVendor = gl.vendor;
    info.apis.openGlIsEs = gl.isEs;
}

std::optional<SmbiosInfo> parseSmbiosTables(const SmbiosTables &tables)
{
    SmbiosVersion version{tables.major, tables.minor};
    if (!tables.entryPoint.isEmpty()) {
        const auto v = parseSmbiosEntryPoint(tables.entryPoint);
        if (!v)
            return std::nullopt;
        version = *v;
    }
    return parseSmbiosTable(tables.table, version);
}

bool applyPrivilegedSmbios(SystemInfo &info, PrivilegedOps &ops, PrivError *error)
{
    const auto tables = ops.readSmbios(error);
    if (!tables)
        return false;
    const auto smbios = parseSmbiosTables(*tables);
    if (!smbios) {
        if (error)
            *error = {PrivError::Code::Protocol, QStringLiteral("Unrecognised SMBIOS entry point")};
        return false;
    }
    applySmbios(info, *smbios);
    return true;
}

namespace {

void applyMemoryDevices(SystemInfo &info, const QVector<SmbiosMemoryDevice> &devices, const QString &source)
{
    info.memory.modules.clear();
    for (const SmbiosMemoryDevice &d : devices) {
        info.memory.modules.append({d.sizeBytes, d.type, d.speedMts, d.configuredSpeedMts, d.manufacturer,
                                    d.partNumber, d.locator, d.bankLocator, d.totalWidth, d.dataWidth,
                                    d.formFactor, d.rank, d.manufacturerSource});
    }
    info.memory.modulesKnown = true;
    info.memory.channels = estimateMemoryChannels(devices);
    info.memory.modulesSource = source;
}

} // namespace

bool applyUdevMemory(SystemInfo &info, const QString &root)
{
    QFile f(QDir(root).filePath(QStringLiteral("run/udev/data/+dmi:id")));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QVector<SmbiosMemoryDevice> devices = parseUdevMemoryDevices(QString::fromUtf8(f.readAll()));
    if (devices.isEmpty())
        return false;
    applyMemoryDevices(info, devices, QStringLiteral("udev"));
    return true;
}

void applySmbios(SystemInfo &info, const SmbiosInfo &smbios)
{
    applyMemoryDevices(info, smbios.memoryDevices, QStringLiteral("smbios"));

    const auto fill = [](QString &field, const QString &value) {
        if (field.isEmpty())
            field = value;
    };
    fill(info.machine.maker, smbios.maker);
    fill(info.machine.model, smbios.product);
    fill(info.machine.family, smbios.family);
    fill(info.machine.biosVendor, smbios.biosVendor);
    fill(info.machine.biosVersion, smbios.biosVersion);
    fill(info.machine.biosDate, smbios.biosDate);
}

} // namespace sb::sysinfo
