// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/linux/sysfs_readers.h"

#include "sysinfo/parsers/text_parsers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace sb::sysinfo::linux_sysfs {

namespace {

QString path(const QString &root, const QString &rel)
{
    return QDir::cleanPath(root + QLatin1Char('/') + rel);
}

// Small sysfs/proc files only; trailing newline and NULs stripped.
QString readText(const QString &file, qint64 maxBytes = 1 << 20)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QByteArray data = f.read(maxBytes);
    while (!data.isEmpty() && (data.endsWith('\0') || data.endsWith('\n')))
        data.chop(1);
    return QString::fromUtf8(data).trimmed();
}

QString read(const QString &root, const QString &rel)
{
    return readText(path(root, rel));
}

qint64 readInt(const QString &root, const QString &rel, qint64 fallback = -1)
{
    bool ok = false;
    const qint64 v = read(root, rel).toLongLong(&ok, 0);
    return ok ? v : fallback;
}

// The kernel driver bound to a device: the last component of its "driver"
// symlink.
QString driverName(const QString &deviceDir)
{
    const QFileInfo link(deviceDir + QStringLiteral("/driver"));
    if (!link.isSymLink())
        return {};
    return QFileInfo(link.symLinkTarget()).fileName();
}

quint32 hexId(const QString &s)
{
    bool ok = false;
    const uint v = s.toUInt(&ok, 16);  // "0x1002"
    return ok ? v : 0;
}

QStringList entries(const QString &dir, const QString &pattern)
{
    const QRegularExpression re(QRegularExpression::anchoredPattern(pattern));
    QStringList out;
    const QStringList names = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name);
    for (const QString &n : names) {
        if (re.match(n).hasMatch())
            out.append(n);
    }
    return out;
}

} // namespace

MachineInfo readMachine(const QString &root)
{
    MachineInfo m;
    const auto dmi = [&](const char *name) {
        return cleanFirmwareString(read(root, QStringLiteral("sys/class/dmi/id/") + QLatin1StringView(name)));
    };
    m.maker = dmi("sys_vendor");
    m.model = dmi("product_name");
    m.family = dmi("product_family");
    m.biosVendor = dmi("bios_vendor");
    m.biosVersion = dmi("bios_version");
    m.biosDate = dmi("bios_date");
    if (m.model.isEmpty())
        m.model = read(root, QStringLiteral("proc/device-tree/model"));  // ARM boards
    return m;
}

CpuInfo readCpu(const QString &root)
{
    CpuInfo c;
    const CpuInfoText text = parseCpuInfo(read(root, QStringLiteral("proc/cpuinfo")));

    QVector<int> cpus = parseCpuList(read(root, QStringLiteral("sys/devices/system/cpu/online")));
    if (cpus.isEmpty()) {
        for (int i = 0; i < text.logicalCpus; ++i)
            cpus.append(i);
    }
    c.threads = int(cpus.size());

    // Cores = distinct (package, die, core id); packages = distinct package ids.
    QSet<qint64> packages;
    std::map<std::tuple<qint64, qint64, qint64>, int> coreFirstCpu;
    QHash<int, std::tuple<qint64, qint64, qint64>> coreOf;
    std::map<qint64, std::set<std::tuple<qint64, qint64, qint64>>> coresByCapacity;
    QMap<QString, int> coreTypeCount;
    qint64 maxKHz = 0;

    struct CacheKey {
        int level;
        QString type, shared;
        bool operator<(const CacheKey &o) const { return std::tie(level, type, shared) < std::tie(o.level, o.type, o.shared); }
    };
    std::map<CacheKey, qint64> caches;

    for (int cpu : std::as_const(cpus)) {
        const QString dir = QStringLiteral("sys/devices/system/cpu/cpu%1/").arg(cpu);
        const qint64 pkg = readInt(root, dir + QStringLiteral("topology/physical_package_id"), 0);
        const qint64 die = readInt(root, dir + QStringLiteral("topology/die_id"), 0);
        const qint64 core = readInt(root, dir + QStringLiteral("topology/core_id"), cpu);
        const auto key = std::make_tuple(pkg, die, core);
        packages.insert(pkg);
        coreFirstCpu.emplace(key, cpu);
        coreOf.insert(cpu, key);

        const qint64 capacity = readInt(root, dir + QStringLiteral("cpu_capacity"), -1);
        if (capacity > 0)
            coresByCapacity[capacity].insert(key);

        maxKHz = std::max(maxKHz, readInt(root, dir + QStringLiteral("cpufreq/cpuinfo_max_freq"), 0));

        if (const auto midr = parseMidr(read(root, dir + QStringLiteral("regs/identification/midr_el1")))) {
            if (coreFirstCpu[key] == cpu)
                ++coreTypeCount[armCoreName(midr->implementer, midr->part)];
        }

        for (const QString &index : entries(path(root, dir + QStringLiteral("cache")), QStringLiteral("index\\d+"))) {
            const QString idx = dir + QStringLiteral("cache/") + index + QLatin1Char('/');
            const CacheKey k{int(readInt(root, idx + QStringLiteral("level"), 0)), read(root, idx + QStringLiteral("type")),
                             read(root, idx + QStringLiteral("shared_cpu_list"))};
            caches.emplace(k, parseCacheSize(read(root, idx + QStringLiteral("size"))));
        }
    }
    c.packages = int(packages.size());
    c.cores = int(coreFirstCpu.size());
    c.maxMhz = int(maxKHz / 1000);
    c.baseMhz = int(readInt(root, QStringLiteral("sys/devices/system/cpu/cpu%1/cpufreq/base_frequency").arg(cpus.value(0)), 0) / 1000);

    for (const auto &[k, size] : caches) {
        if (k.level == 1 && k.type == QLatin1StringView("Data"))
            c.caches.l1d += size;
        else if (k.level == 1 && k.type == QLatin1StringView("Instruction"))
            c.caches.l1i += size;
        else if (k.level == 2)
            c.caches.l2 += size;
        else if (k.level == 3)
            c.caches.l3 += size;
    }

    // Hybrid: Intel exposes cpu_core / cpu_atom PMUs; ARM big.LITTLE shows
    // as different cpu_capacity values (the top tier counts as performance).
    const QVector<int> pCpus = parseCpuList(read(root, QStringLiteral("sys/devices/cpu_core/cpus")));
    const QVector<int> eCpus = parseCpuList(read(root, QStringLiteral("sys/devices/cpu_atom/cpus")));
    if (!pCpus.isEmpty() && !eCpus.isEmpty()) {
        std::set<std::tuple<qint64, qint64, qint64>> p, e;
        for (int cpu : pCpus)
            if (coreOf.contains(cpu)) p.insert(coreOf[cpu]);
        for (int cpu : eCpus)
            if (coreOf.contains(cpu)) e.insert(coreOf[cpu]);
        c.performanceCores = int(p.size());
        c.efficiencyCores = int(e.size());
    } else if (coresByCapacity.size() > 1) {
        c.performanceCores = int(coresByCapacity.rbegin()->second.size());
        c.efficiencyCores = c.cores - c.performanceCores;
    }

    if (text.isArm() || !coreTypeCount.isEmpty()) {
        c.arch = QStringLiteral("arm64");
        if (coreTypeCount.isEmpty()) {
            for (const ArmCoreId &id : text.armCores)
                coreTypeCount[armCoreName(id.implementer, id.part)] += 0;
        }
        int implementer = text.armCores.isEmpty() ? -1 : text.armCores.first().implementer;
        if (implementer < 0) {
            const auto midr = parseMidr(read(root, QStringLiteral("sys/devices/system/cpu/cpu%1/regs/identification/midr_el1").arg(cpus.value(0))));
            implementer = midr ? midr->implementer : -1;
        }
        c.vendor = implementer >= 0 ? armVendorName(implementer) : QString();
        c.coreTypes = coreTypeCount.keys();
        // Biggest designs first isn't knowable from names; keep a stable order.
        c.name = c.vendor + QLatin1Char(' ') + c.coreTypes.join(QStringLiteral(" + "));
        c.features = normalizeArmFeatures(text.flags);
    } else {
        c.arch = QStringLiteral("x86_64");
        c.name = text.modelName.simplified();
        c.vendor = text.vendorId == QLatin1StringView("AuthenticAMD")   ? QStringLiteral("AMD")
                   : text.vendorId == QLatin1StringView("GenuineIntel") ? QStringLiteral("Intel")
                                                                        : text.vendorId;
        c.features = normalizeX86Flags(text.flags);
    }
    return c;
}

qint64 readMemTotal(const QString &root)
{
    return parseMemTotal(read(root, QStringLiteral("proc/meminfo")));
}

OsInfo readOsRelease(const QString &root)
{
    QString text = read(root, QStringLiteral("etc/os-release"));
    if (text.isEmpty())
        text = read(root, QStringLiteral("usr/lib/os-release"));
    const auto kv = parseOsRelease(text);
    OsInfo os;
    os.name = kv.value(QStringLiteral("PRETTY_NAME"), kv.value(QStringLiteral("NAME")));
    os.version = kv.value(QStringLiteral("VERSION_ID"), kv.value(QStringLiteral("VERSION")));
    os.build = kv.value(QStringLiteral("BUILD_ID"));
    return os;
}

QVector<GpuInfo> readGpus(const QString &root)
{
    QVector<GpuInfo> out;
    QSet<QString> seen;
    const QString drm = path(root, QStringLiteral("sys/class/drm"));
    for (const QString &card : entries(drm, QStringLiteral("card\\d+"))) {
        const QString dev = drm + QLatin1Char('/') + card + QStringLiteral("/device");
        const QString real = QFileInfo(dev).canonicalFilePath();
        if (real.isEmpty() || seen.contains(real))
            continue;
        seen.insert(real);
        GpuInfo g;
        g.vendorId = hexId(readText(dev + QStringLiteral("/vendor")));
        g.deviceId = hexId(readText(dev + QStringLiteral("/device")));
        if (!g.vendorId)
            continue;
        g.vendor = pciVendorName(g.vendorId);
        g.driver = driverName(dev);
        bool ok = false;
        const qint64 vram = readText(dev + QStringLiteral("/mem_info_vram_total")).toLongLong(&ok);
        g.vramBytes = ok ? vram : 0;
        out.append(g);
    }
    return out;
}

QVector<NpuInfo> readNpus(const QString &root)
{
    QVector<NpuInfo> out;
    const QString accel = path(root, QStringLiteral("sys/class/accel"));
    for (const QString &name : entries(accel, QStringLiteral("accel\\d+"))) {
        const QString dev = accel + QLatin1Char('/') + name + QStringLiteral("/device");
        NpuInfo n;
        n.driver = driverName(dev);
        n.vendorId = hexId(readText(dev + QStringLiteral("/vendor")));
        n.deviceId = hexId(readText(dev + QStringLiteral("/device")));
        n.vendor = pciVendorName(n.vendorId);
        if (n.driver == QLatin1StringView("amdxdna"))
            n.name = QStringLiteral("AMD XDNA NPU");
        else if (n.driver == QLatin1StringView("intel_vpu"))
            n.name = QStringLiteral("Intel NPU");
        else
            n.name = n.vendor.isEmpty() ? QStringLiteral("NPU") : n.vendor + QStringLiteral(" NPU");
        if (!n.driver.isEmpty())
            n.driverVersion = read(root, QStringLiteral("sys/module/%1/version").arg(n.driver));
        out.append(n);
    }
    return out;
}

BatteryInfo readBattery(const QString &root)
{
    BatteryInfo b;
    const QString base = path(root, QStringLiteral("sys/class/power_supply"));
    double energyNow = 0, energyFull = 0;
    bool haveCapacity = false;
    double capacitySum = 0;
    int capacityCount = 0;
    const QStringList supplies = QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name);
    for (const QString &s : supplies) {
        const QString dir = base + QLatin1Char('/') + s + QLatin1Char('/');
        if (readText(dir + QStringLiteral("scope")) == QLatin1StringView("Device"))
            continue;  // a mouse or a gamepad, not the machine
        const QString type = readText(dir + QStringLiteral("type"));
        if (type == QLatin1StringView("Mains") || type == QLatin1StringView("USB")) {
            if (readText(dir + QStringLiteral("online")) == QLatin1StringView("1"))
                b.acOnline = true;
            continue;
        }
        if (type != QLatin1StringView("Battery") || readText(dir + QStringLiteral("present")) == QLatin1StringView("0"))
            continue;

        const auto num = [&](const char *f) {
            bool ok = false;
            const double v = readText(dir + QLatin1StringView(f)).toDouble(&ok);
            return ok ? v : -1.0;
        };
        double full = num("energy_full"), design = num("energy_full_design"), now = num("energy_now");
        if (full < 0 || design < 0) {
            // charge_* (µAh) × voltage_min_design (µV)
            const double v = num("voltage_min_design");
            if (v > 0) {
                const double cf = num("charge_full"), cd = num("charge_full_design"), cn = num("charge_now");
                full = cf >= 0 ? cf * v / 1e6 : -1;
                design = cd >= 0 ? cd * v / 1e6 : -1;
                now = cn >= 0 ? cn * v / 1e6 : -1;
            }
        }
        b.present = true;
        if (design > 0)
            b.designWh += design / 1e6;  // µWh -> Wh
        if (full > 0) {
            b.fullWh += full / 1e6;
            energyFull += full;
        }
        if (now >= 0)
            energyNow += now;
        const double cap = num("capacity");
        if (cap >= 0) {
            haveCapacity = true;
            capacitySum += cap;
            ++capacityCount;
        }
        const double cycles = num("cycle_count");
        if (cycles >= 0)
            b.cycleCount = std::max(b.cycleCount, 0) + int(cycles);
    }
    if (b.present) {
        if (b.designWh > 0 && b.fullWh > 0)
            b.healthPct = b.fullWh / b.designWh * 100.0;
        if (energyFull > 0 && energyNow >= 0)
            b.chargePct = std::min(100.0, energyNow / energyFull * 100.0);
        else if (haveCapacity)
            b.chargePct = capacitySum / capacityCount;
    }
    return b;
}

BlockDeviceInfo readBlockDevice(const QString &root, unsigned major, unsigned minor)
{
    BlockDeviceInfo info;
    const QString entry = path(root, QStringLiteral("sys/dev/block/%1:%2").arg(major).arg(minor));
    QString real = QFileInfo(entry).canonicalFilePath();
    if (real.isEmpty())
        return info;
    if (QFileInfo::exists(real + QStringLiteral("/partition")))
        real = QFileInfo(real).path();  // the whole disk
    info.model = readText(real + QStringLiteral("/device/model")).simplified();
    const QString rotational = readText(real + QStringLiteral("/queue/rotational"));
    if (rotational == QLatin1StringView("0"))
        info.kind = QStringLiteral("SSD");
    else if (rotational == QLatin1StringView("1"))
        info.kind = QStringLiteral("HDD");

    if (real.contains(QLatin1StringView("/nvme/")))
        info.bus = QStringLiteral("NVMe");
    else if (real.contains(QLatin1StringView("/usb")))
        info.bus = QStringLiteral("USB");
    else if (real.contains(QLatin1StringView("/ata")))
        info.bus = QStringLiteral("SATA");
    else if (real.contains(QLatin1StringView("/mmc")))
        info.bus = QStringLiteral("MMC");
    else if (real.contains(QLatin1StringView("/virtio")))
        info.bus = QStringLiteral("Virtio");
    else if (real.contains(QLatin1StringView("/virtual/")))
        info.bus = QStringLiteral("Virtual");
    if (info.bus == QLatin1StringView("Virtual"))
        info.kind.clear();  // zram/loop: rotational=0 means nothing
    return info;
}

} // namespace sb::sysinfo::linux_sysfs
