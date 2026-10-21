// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/system_info.h"

#include <QJsonArray>

namespace sb {

namespace {

QJsonValue opt(const QString &s)
{
    return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s);
}

QJsonValue opt(qint64 v)
{
    return v > 0 ? QJsonValue(double(v)) : QJsonValue(QJsonValue::Null);
}

QJsonValue optInt(int v)
{
    return v > 0 ? QJsonValue(v) : QJsonValue(QJsonValue::Null);
}

QString hex4(quint32 v)
{
    return QStringLiteral("%1").arg(v, 4, 16, QLatin1Char('0'));
}

} // namespace

QString SystemInfo::machineLabel() const
{
    // "Knowhere · ASUS TUF Gaming A14" (Dave): host name + maker and the
    // marketing family when the firmware has one (else the model). Either
    // half alone if the other is unknown.
    QString hardware = machine.family.isEmpty() ? machine.model : machine.family;
    if (!machine.maker.isEmpty() && !hardware.startsWith(machine.maker, Qt::CaseInsensitive))
        hardware = machine.maker + QLatin1Char(' ') + hardware;
    hardware = hardware.trimmed();
    if (!machine.hostname.isEmpty() && !hardware.isEmpty())
        return machine.hostname + QStringLiteral(" · ") + hardware;
    if (!machine.hostname.isEmpty())
        return machine.hostname;
    return hardware.isEmpty() ? QStringLiteral("Unknown machine") : hardware;
}

DisplayInfo displayFromSnapshotData(const QJsonObject &data)
{
    DisplayInfo d;
    const QJsonObject o = data.value(QStringLiteral("display")).toObject();
    d.name = o.value(QStringLiteral("name")).toString();
    d.availWidth = o.value(QStringLiteral("avail_width")).toInt();
    d.availHeight = o.value(QStringLiteral("avail_height")).toInt();
    d.scale = o.contains(QStringLiteral("scale")) ? o.value(QStringLiteral("scale")).toDouble(1.0) : 1.0;
    return d;
}

QJsonObject SystemInfo::toJson() const
{
    QJsonObject j;
    j.insert(QStringLiteral("machine"),
             QJsonObject{{QStringLiteral("maker"), opt(machine.maker)},
                         {QStringLiteral("model"), opt(machine.model)},
                         {QStringLiteral("family"), opt(machine.family)},
                         {QStringLiteral("bios_vendor"), opt(machine.biosVendor)},
                         {QStringLiteral("bios_version"), opt(machine.biosVersion)},
                         {QStringLiteral("bios_date"), opt(machine.biosDate)},
                         {QStringLiteral("hostname"), opt(machine.hostname)}});

    j.insert(QStringLiteral("cpu"),
             QJsonObject{{QStringLiteral("name"), opt(cpu.name)},
                         {QStringLiteral("vendor"), opt(cpu.vendor)},
                         {QStringLiteral("arch"), opt(cpu.arch)},
                         {QStringLiteral("packages"), optInt(cpu.packages)},
                         {QStringLiteral("cores"), optInt(cpu.cores)},
                         {QStringLiteral("threads"), optInt(cpu.threads)},
                         {QStringLiteral("performance_cores"), optInt(cpu.performanceCores)},
                         {QStringLiteral("efficiency_cores"), optInt(cpu.efficiencyCores)},
                         {QStringLiteral("core_types"), QJsonArray::fromStringList(cpu.coreTypes)},
                         {QStringLiteral("cache_bytes"),
                          QJsonObject{{QStringLiteral("l1d"), opt(cpu.caches.l1d)},
                                      {QStringLiteral("l1i"), opt(cpu.caches.l1i)},
                                      {QStringLiteral("l2"), opt(cpu.caches.l2)},
                                      {QStringLiteral("l3"), opt(cpu.caches.l3)}}},
                         {QStringLiteral("base_mhz"), optInt(cpu.baseMhz)},
                         {QStringLiteral("max_mhz"), optInt(cpu.maxMhz)},
                         {QStringLiteral("features"), QJsonArray::fromStringList(cpu.features)}});

    QJsonArray modules;
    for (const MemoryModule &m : memory.modules) {
        modules.append(QJsonObject{{QStringLiteral("size_bytes"), opt(m.sizeBytes)},
                                   {QStringLiteral("type"), opt(m.type)},
                                   {QStringLiteral("speed_mts"), optInt(m.speedMts)},
                                   {QStringLiteral("configured_speed_mts"), optInt(m.configuredSpeedMts)},
                                   {QStringLiteral("manufacturer"), opt(m.manufacturer)},
                                   {QStringLiteral("part_number"), opt(m.partNumber)},
                                   {QStringLiteral("locator"), opt(m.locator)},
                                   {QStringLiteral("bank_locator"), opt(m.bankLocator)},
                                   {QStringLiteral("rank"), optInt(m.rank)},
                                   {QStringLiteral("data_width"), optInt(m.dataWidth)},
                                   {QStringLiteral("total_width"), optInt(m.totalWidth)},
                                   {QStringLiteral("form_factor"), opt(m.formFactor)},
                                   {QStringLiteral("manufacturer_source"), opt(m.manufacturerSource)}});
    }
    j.insert(QStringLiteral("memory"),
             QJsonObject{{QStringLiteral("total_bytes"), opt(memory.totalBytes)},
                         {QStringLiteral("modules_known"), memory.modulesKnown},
                         {QStringLiteral("modules"), modules},
                         {QStringLiteral("modules_source"), opt(memory.modulesSource)},
                         {QStringLiteral("channels"), optInt(memory.channels)}});

    QJsonArray gpuArray;
    for (const GpuInfo &g : gpus) {
        gpuArray.append(QJsonObject{{QStringLiteral("name"), opt(g.name)},
                                    {QStringLiteral("vendor"), opt(g.vendor)},
                                    {QStringLiteral("pci_id"), QStringLiteral("%1:%2").arg(hex4(g.vendorId), hex4(g.deviceId))},
                                    {QStringLiteral("driver"), opt(g.driver)},
                                    {QStringLiteral("driver_version"), opt(g.driverVersion)},
                                    {QStringLiteral("vram_bytes"), opt(g.vramBytes)},
                                    {QStringLiteral("vulkan_version"), opt(g.vulkanVersion)}});
    }
    j.insert(QStringLiteral("gpus"), gpuArray);

    j.insert(QStringLiteral("apis"),
             QJsonObject{{QStringLiteral("vulkan_instance_version"), opt(apis.vulkanInstanceVersion)},
                         {QStringLiteral("opengl_version"), opt(apis.openGlVersion)},
                         {QStringLiteral("opengl_renderer"), opt(apis.openGlRenderer)},
                         {QStringLiteral("opengl_vendor"), opt(apis.openGlVendor)},
                         {QStringLiteral("opengl_is_es"), apis.openGlIsEs},
                         {QStringLiteral("d3d12_feature_level"), opt(apis.d3d12FeatureLevel)},
                         {QStringLiteral("d3d11_feature_level"), opt(apis.d3d11FeatureLevel)}});

    j.insert(QStringLiteral("display"),
             QJsonObject{{QStringLiteral("name"), opt(display.name)},
                         {QStringLiteral("avail_width"), display.availWidth},
                         {QStringLiteral("avail_height"), display.availHeight},
                         {QStringLiteral("scale"), display.scale}});

    QJsonArray vols;
    for (const VolumeInfo &v : volumes) {
        vols.append(QJsonObject{{QStringLiteral("mount"), v.mount},
                                {QStringLiteral("file_system"), opt(v.fileSystem)},
                                {QStringLiteral("total_bytes"), opt(v.totalBytes)},
                                {QStringLiteral("free_bytes"), double(v.freeBytes)},
                                {QStringLiteral("model"), opt(v.model)},
                                {QStringLiteral("bus"), opt(v.bus)},
                                {QStringLiteral("kind"), opt(v.kind)}});
    }
    j.insert(QStringLiteral("storage"), vols);

    j.insert(QStringLiteral("os"),
             QJsonObject{{QStringLiteral("name"), opt(os.name)},
                         {QStringLiteral("version"), opt(os.version)},
                         {QStringLiteral("build"), opt(os.build)},
                         {QStringLiteral("kernel"), opt(os.kernel)},
                         {QStringLiteral("arch"), opt(os.arch)},
                         {QStringLiteral("session"), opt(os.session)}});

    QJsonArray npuArray;
    for (const NpuInfo &n : npus) {
        npuArray.append(QJsonObject{{QStringLiteral("name"), opt(n.name)},
                                    {QStringLiteral("vendor"), opt(n.vendor)},
                                    {QStringLiteral("driver"), opt(n.driver)},
                                    {QStringLiteral("driver_version"), opt(n.driverVersion)},
                                    {QStringLiteral("pci_id"), QStringLiteral("%1:%2").arg(hex4(n.vendorId), hex4(n.deviceId))}});
    }
    j.insert(QStringLiteral("npus"), npuArray);

    j.insert(QStringLiteral("battery"),
             QJsonObject{{QStringLiteral("present"), battery.present},
                         {QStringLiteral("design_wh"), battery.present ? QJsonValue(battery.designWh) : QJsonValue()},
                         {QStringLiteral("full_wh"), battery.present ? QJsonValue(battery.fullWh) : QJsonValue()},
                         {QStringLiteral("health_pct"), battery.present ? QJsonValue(battery.healthPct) : QJsonValue()},
                         {QStringLiteral("charge_pct"), battery.chargePct >= 0 ? QJsonValue(battery.chargePct) : QJsonValue()},
                         {QStringLiteral("ac_online"), battery.acOnline},
                         {QStringLiteral("cycle_count"), battery.cycleCount >= 0 ? QJsonValue(battery.cycleCount) : QJsonValue()}});

    j.insert(QStringLiteral("app"),
             QJsonObject{{QStringLiteral("version"), app.version},
                         {QStringLiteral("build"), app.build},
                         {QStringLiteral("qt_version"), app.qtVersion},
                         {QStringLiteral("compiler"), app.compiler},
                         {QStringLiteral("git_sha"), app.gitSha}});

    j.insert(QStringLiteral("emulated"), emulated);
    j.insert(QStringLiteral("native_arch"), opt(nativeArch));
    return j;
}

SystemSnapshot SystemInfo::toSnapshot() const
{
    SystemSnapshot s;
    s.machineLabel = machineLabel();
    s.data = toJson();
    return s;
}

} // namespace sb
