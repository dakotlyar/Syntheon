// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/parsers/windows_parsers.h"

#include "sysinfo/parsers/text_parsers.h"

#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <algorithm>

namespace sb::sysinfo::win {

std::optional<std::pair<quint32, quint32>> parseHardwareId(const QString &hardwareId)
{
    static const QRegularExpression re(QStringLiteral(R"(VEN_([0-9A-F]{4}).*DEV_([0-9A-F]{4}))"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(hardwareId);
    if (!m.hasMatch())
        return std::nullopt;
    return std::make_pair(m.captured(1).toUInt(nullptr, 16), m.captured(2).toUInt(nullptr, 16));
}

QString busTypeName(int busType)
{
    switch (busType) {
    case 1: return QStringLiteral("SCSI");
    case 2: return QStringLiteral("ATAPI");
    case 3: return QStringLiteral("ATA");
    case 4: return QStringLiteral("IEEE 1394");
    case 5: return QStringLiteral("SSA");
    case 6: return QStringLiteral("Fibre");
    case 7: return QStringLiteral("USB");
    case 8: return QStringLiteral("RAID");
    case 9: return QStringLiteral("iSCSI");
    case 10: return QStringLiteral("SAS");
    case 11: return QStringLiteral("SATA");
    case 12: return QStringLiteral("SD");
    case 13: return QStringLiteral("MMC");
    case 14: return QStringLiteral("Virtual");
    case 15: return QStringLiteral("File-backed virtual");
    case 16: return QStringLiteral("Storage Spaces");
    case 17: return QStringLiteral("NVMe");
    case 18: return QStringLiteral("SCM");
    case 19: return QStringLiteral("UFS");
    default: return {};
    }
}

QString StorageDescriptor::model() const
{
    const QString v = vendor.simplified();
    const QString p = product.simplified();
    if (v.isEmpty() || p.startsWith(v, Qt::CaseInsensitive))
        return p;
    return v + QLatin1Char(' ') + p;
}

std::optional<StorageDescriptor> parseStorageDeviceDescriptor(const QByteArray &buffer)
{
    // Version(4) Size(4) DeviceType(1) Modifier(1) Removable(1) Queueing(1)
    // VendorIdOffset(4)@12 ProductIdOffset(4)@16 RevisionOffset(4)@20
    // SerialNumberOffset(4)@24 (never read) BusType(4)@28 RawLength(4)@32
    if (buffer.size() < 36)
        return std::nullopt;
    const auto *p = reinterpret_cast<const uchar *>(buffer.constData());
    const qsizetype size = std::min<qsizetype>(buffer.size(), qFromLittleEndian<quint32>(p + 4));
    const auto stringAt = [&](int fieldOffset) -> QString {
        const quint32 off = qFromLittleEndian<quint32>(p + fieldOffset);
        if (off == 0 || off >= size)
            return {};
        qsizetype end = off;
        while (end < size && p[end] != 0)
            ++end;
        return QString::fromLatin1(reinterpret_cast<const char *>(p + off), end - off).trimmed();
    };
    StorageDescriptor d;
    d.vendor = stringAt(12);
    d.product = stringAt(16);
    d.busType = int(qFromLittleEndian<quint32>(p + 28));
    return d;
}

QString windowsName(const QString &productName, int build)
{
    QString name = productName.simplified();
    if (build >= 22000 && name.startsWith(QLatin1StringView("Windows 10")))
        name.replace(0, 10, QStringLiteral("Windows 11"));
    return name;
}

QString dxgiDriverVersion(qint64 umd)
{
    const quint64 v = quint64(umd);
    return QStringLiteral("%1.%2.%3.%4").arg(v >> 48).arg((v >> 32) & 0xffff).arg((v >> 16) & 0xffff).arg(v & 0xffff);
}

QString featureLevelName(int level)
{
    if (level <= 0)
        return {};
    return QStringLiteral("%1_%2").arg((level >> 12) & 0xf).arg((level >> 8) & 0xf);
}

QString emulatedNativeArch(bool buildIsX64, int nativeMachine)
{
    return buildIsX64 && nativeMachine == 0xAA64 ? QStringLiteral("arm64") : QString();
}

BatteryInfo combineBattery(const QVector<BatteryFields> &batteries, int acLineStatus, int lifePercent,
                           int batteryFlag)
{
    BatteryInfo b;
    b.acOnline = acLineStatus == 1;
    b.present = !batteries.isEmpty() || (batteryFlag != 128 && batteryFlag != 255);
    if (lifePercent >= 0 && lifePercent <= 100 && b.present)
        b.chargePct = lifePercent;
    double design = 0, full = 0;
    for (const BatteryFields &f : batteries) {
        if (!f.relative) {
            design += f.designedCapacity / 1000.0;
            full += f.fullChargedCapacity / 1000.0;
        }
        if (f.cycleCount > 0)
            b.cycleCount = std::max(b.cycleCount, 0) + int(f.cycleCount);
    }
    b.designWh = design;
    b.fullWh = full;
    if (design > 0 && full > 0)
        b.healthPct = full / design * 100.0;
    return b;
}

void countCores(const QVector<CoreEntry> &cores, CpuInfo &cpu)
{
    cpu.cores = int(cores.size());
    cpu.threads = 0;
    int top = -1;
    QSet<int> classes;
    for (const CoreEntry &c : cores) {
        cpu.threads += c.logicalProcessors;
        classes.insert(c.efficiencyClass);
        top = std::max(top, c.efficiencyClass);
    }
    if (classes.size() > 1) {
        cpu.performanceCores = int(std::count_if(cores.begin(), cores.end(),
                                                 [top](const CoreEntry &c) { return c.efficiencyClass == top; }));
        cpu.efficiencyCores = cpu.cores - cpu.performanceCores;
    }
}

CacheSizes sumCaches(const QVector<CacheEntry> &caches)
{
    CacheSizes s;
    for (const CacheEntry &c : caches) {
        if (c.level == 1 && c.type == 2)
            s.l1d += c.size;
        else if (c.level == 1 && c.type == 1)
            s.l1i += c.size;
        else if (c.level == 2)
            s.l2 += c.size;
        else if (c.level == 3)
            s.l3 += c.size;
    }
    return s;
}

} // namespace sb::sysinfo::win
