// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/collector.h"

#include "sysinfo/common/vulkan_probe.h"
#include "sysinfo/common/x86_cpu.h"
#include "sysinfo/linux/sysfs_readers.h"
#include "sysinfo/parsers/text_parsers.h"

#include <QHash>
#include <QStorageInfo>
#include <QSysInfo>

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

namespace sb::sysinfo {

namespace {

QString hostName()
{
    char buf[256] = {};
    if (gethostname(buf, sizeof buf - 1) != 0)
        return {};
    return QString::fromLocal8Bit(buf);
}

#if defined(__aarch64__)
QStringList armHwcaps()
{
    const unsigned long h = getauxval(AT_HWCAP);
    const unsigned long h2 = getauxval(AT_HWCAP2);
    QStringList raw;
    if (h & HWCAP_ASIMD) raw << QStringLiteral("asimd");
    if (h & HWCAP_ASIMDHP) raw << QStringLiteral("asimdhp");
    if (h & HWCAP_ASIMDDP) raw << QStringLiteral("asimddp");
    if (h & HWCAP_SVE) raw << QStringLiteral("sve");
    if (h & HWCAP_CRC32) raw << QStringLiteral("crc32");
    if (h & HWCAP_AES) raw << QStringLiteral("aes");
    if (h & HWCAP_SHA2) raw << QStringLiteral("sha2");
    if (h & HWCAP_ATOMICS) raw << QStringLiteral("atomics");
#ifdef HWCAP2_SVE2
    if (h2 & HWCAP2_SVE2) raw << QStringLiteral("sve2");
#endif
#ifdef HWCAP2_I8MM
    if (h2 & HWCAP2_I8MM) raw << QStringLiteral("i8mm");
#endif
#ifdef HWCAP2_BF16
    if (h2 & HWCAP2_BF16) raw << QStringLiteral("bf16");
#endif
#ifdef HWCAP2_SME
    if (h2 & HWCAP2_SME) raw << QStringLiteral("sme");
#endif
    (void)h2;
    return normalizeArmFeatures(raw);
}
#endif

QVector<VolumeInfo> volumes()
{
    // Real, mounted file systems, one entry per device (btrfs subvolumes and
    // bind mounts share a device; the shortest mount point wins).
    QHash<QString, VolumeInfo> byDevice;
    for (const QStorageInfo &s : QStorageInfo::mountedVolumes()) {
        const QString device = QString::fromLocal8Bit(s.device());
        if (!s.isValid() || !s.isReady() || !device.startsWith(QLatin1StringView("/dev/"))
            || device.startsWith(QLatin1StringView("/dev/loop")))
            continue;
        VolumeInfo v;
        v.mount = s.rootPath();
        v.fileSystem = QString::fromLatin1(s.fileSystemType());
        v.totalBytes = s.bytesTotal();
        v.freeBytes = s.bytesAvailable();
        struct stat st {};
        if (::stat(s.device().constData(), &st) == 0 && S_ISBLK(st.st_mode)) {
            const auto dev = linux_sysfs::readBlockDevice(QStringLiteral("/"), major(st.st_rdev), minor(st.st_rdev));
            v.model = dev.model;
            v.bus = dev.bus;
            v.kind = dev.kind;
        }
        const auto it = byDevice.constFind(device);
        if (it == byDevice.cend() || v.mount.size() < it->mount.size())
            byDevice.insert(device, v);
    }
    QVector<VolumeInfo> out = byDevice.values().toVector();
    std::sort(out.begin(), out.end(), [](const VolumeInfo &a, const VolumeInfo &b) { return a.mount < b.mount; });
    return out;
}

} // namespace

BatteryInfo readBatteryNow()
{
    return linux_sysfs::readBattery(QStringLiteral("/"));
}

SystemInfo collectSystemInfo()
{
    const QString root = QStringLiteral("/");
    SystemInfo info;
    info.machine = linux_sysfs::readMachine(root);
    info.machine.hostname = hostName();

    info.cpu = linux_sysfs::readCpu(root);
#if defined(__x86_64__)
    const X86Cpu x86 = queryX86Cpu();
    if (!x86.features.isEmpty())
        info.cpu.features = x86.features;  // CPUID + OS support, not just flags
    if (info.cpu.name.isEmpty())
        info.cpu.name = x86.brand;
#elif defined(__aarch64__)
    info.cpu.features = armHwcaps();
#endif

    info.memory.totalBytes = linux_sysfs::readMemTotal(root);
    // The modules' baseline until the helper's SMBIOS tables arrive (they
    // replace it), and what stays if it never does: udev's world-readable
    // copy (Phase 3 D11). It holds the same type-17 fields.
    applyUdevMemory(info, root);

    info.gpus = linux_sysfs::readGpus(root);
    const VulkanInfo vk = probeVulkan();
    info.apis.vulkanInstanceVersion = vk.instanceVersion;
    for (GpuInfo &g : info.gpus) {
        for (const VulkanDevice &d : vk.devices) {
            if (d.vendorId == g.vendorId && d.deviceId == g.deviceId) {
                g.name = d.name;
                g.vulkanVersion = d.apiVersion;
                g.driverVersion = d.driverInfo;
                if (g.vramBytes == 0)
                    g.vramBytes = d.deviceLocalBytes;
                break;
            }
        }
        if (g.name.isEmpty())
            g.name = QStringLiteral("%1 GPU %2:%3")
                         .arg(g.vendor.isEmpty() ? QStringLiteral("PCI") : g.vendor)
                         .arg(g.vendorId, 4, 16, QLatin1Char('0'))
                         .arg(g.deviceId, 4, 16, QLatin1Char('0'));
    }

    info.volumes = volumes();

    info.os = linux_sysfs::readOsRelease(root);
    struct utsname u {};
    if (uname(&u) == 0)
        info.os.kernel = QString::fromLatin1(u.release);
    info.os.arch = QSysInfo::currentCpuArchitecture();
    info.os.session = qEnvironmentVariable("XDG_SESSION_TYPE");

    info.npus = linux_sysfs::readNpus(root);
    info.battery = linux_sysfs::readBattery(root);
    info.app = appInfo();
    return info;
}

} // namespace sb::sysinfo
