// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/collector.h"

#include "sysinfo/common/opengl_probe.h"

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

void applySmbios(SystemInfo &info, const SmbiosInfo &smbios)
{
    info.memory.modules.clear();
    for (const SmbiosMemoryDevice &d : smbios.memoryDevices) {
        info.memory.modules.append({d.sizeBytes, d.type, d.speedMts, d.configuredSpeedMts, d.manufacturer,
                                    d.partNumber, d.locator, d.bankLocator});
    }
    info.memory.modulesKnown = true;
    info.memory.channels = estimateMemoryChannels(smbios.memoryDevices);

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
