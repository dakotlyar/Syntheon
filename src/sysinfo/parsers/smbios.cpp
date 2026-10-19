// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/parsers/smbios.h"

#include "sysinfo/parsers/text_parsers.h"

#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <map>

namespace sb::sysinfo {

namespace {

// One structure: the formatted area plus its string set.
struct Structure {
    const uchar *data = nullptr;
    int length = 0;  // formatted area length (header included)
    QVector<QByteArray> strings;

    int type() const { return data[0]; }
    bool has(int offset, int size) const { return offset >= 0 && offset + size <= length; }
    int byte(int offset) const { return has(offset, 1) ? data[offset] : -1; }
    int word(int offset) const { return has(offset, 2) ? qFromLittleEndian<quint16>(data + offset) : -1; }
    qint64 dword(int offset) const { return has(offset, 4) ? qint64(qFromLittleEndian<quint32>(data + offset)) : -1; }
    QString string(int offset) const
    {
        const int index = byte(offset);
        if (index <= 0 || index > strings.size())
            return {};
        return cleanFirmwareString(QString::fromLatin1(strings[index - 1]));
    }
};

QVector<Structure> split(const QByteArray &table)
{
    QVector<Structure> out;
    const auto *base = reinterpret_cast<const uchar *>(table.constData());
    const qsizetype n = table.size();
    qsizetype pos = 0;
    while (pos + 4 <= n) {
        Structure s;
        s.data = base + pos;
        s.length = base[pos + 1];
        if (s.length < 4 || pos + s.length > n)
            break;  // malformed: stop rather than guess
        // The string set follows, terminated by a double NUL.
        qsizetype p = pos + s.length;
        if (p + 1 < n && base[p] == 0 && base[p + 1] == 0) {
            p += 2;  // no strings
        } else {
            bool terminated = false;
            while (p < n) {
                qsizetype end = p;
                while (end < n && base[end] != 0)
                    ++end;
                if (end >= n)
                    break;
                s.strings.append(QByteArray(reinterpret_cast<const char *>(base + p), end - p));
                p = end + 1;
                if (p < n && base[p] == 0) {
                    ++p;
                    terminated = true;
                    break;
                }
            }
            if (!terminated)
                break;
        }
        out.append(s);
        if (s.type() == 127)
            break;  // end-of-table
        pos = p;
    }
    return out;
}

bool atLeast(SmbiosVersion v, int major, int minor)
{
    return v.major > major || (v.major == major && v.minor >= minor);
}

SmbiosMemoryDevice memoryDevice(const Structure &s, SmbiosVersion v)
{
    SmbiosMemoryDevice d;
    const int size = s.word(0x0C);
    if (size == 0x7FFF && s.has(0x1C, 4)) {
        d.sizeBytes = (s.dword(0x1C) & 0x7FFFFFFF) * 1024 * 1024;  // extended size, MB
    } else if (size > 0 && size != 0xFFFF) {
        const qint64 units = size & 0x7FFF;
        d.sizeBytes = (size & 0x8000) ? units * 1024 : units * 1024 * 1024;  // KB or MB
    }
    const auto width = [&](int offset) {
        const int w = s.word(offset);
        return w > 0 && w != 0xFFFF ? w : 0;
    };
    d.totalWidth = width(0x08);
    d.dataWidth = width(0x0A);
    d.formFactor = smbiosFormFactorName(s.byte(0x0E));
    d.locator = s.string(0x10);
    d.bankLocator = s.string(0x11);
    d.type = smbiosMemoryTypeName(s.byte(0x12));

    int speed = s.word(0x15);
    if (speed == 0xFFFF && atLeast(v, 3, 3) && s.has(0x54, 4))
        speed = int(s.dword(0x54));
    d.speedMts = speed > 0 && speed != 0xFFFF ? speed : 0;

    d.manufacturer = s.string(0x17);
    // 0x18 serial number and 0x19 asset tag: never read (P23).
    d.partNumber = s.string(0x1A);

    int configured = s.word(0x20);
    if (configured == 0xFFFF && atLeast(v, 3, 3) && s.has(0x58, 4))
        configured = int(s.dword(0x58));
    d.configuredSpeedMts = configured > 0 && configured != 0xFFFF ? configured : 0;
    if (const int attributes = s.byte(0x1B); attributes > 0)
        d.rank = attributes & 0x0F;
    return d;
}

} // namespace

QString smbiosMemoryTypeName(int type)
{
    switch (type) {
    case 0x12: return QStringLiteral("DDR");
    case 0x13: return QStringLiteral("DDR2");
    case 0x18: return QStringLiteral("DDR3");
    case 0x1A: return QStringLiteral("DDR4");
    case 0x1B: return QStringLiteral("LPDDR");
    case 0x1C: return QStringLiteral("LPDDR2");
    case 0x1D: return QStringLiteral("LPDDR3");
    case 0x1E: return QStringLiteral("LPDDR4");
    case 0x20: return QStringLiteral("HBM");
    case 0x21: return QStringLiteral("HBM2");
    case 0x22: return QStringLiteral("DDR5");
    case 0x23: return QStringLiteral("LPDDR5");
    case 0x24: return QStringLiteral("HBM3");
    default: return {};
    }
}

QString smbiosFormFactorName(int formFactor)
{
    switch (formFactor) {
    case 0x03: return QStringLiteral("SIMM");
    case 0x04: return QStringLiteral("SIP");
    case 0x05: return QStringLiteral("Chip");
    case 0x06: return QStringLiteral("DIP");
    case 0x07: return QStringLiteral("ZIP");
    case 0x08: return QStringLiteral("Proprietary Card");
    case 0x09: return QStringLiteral("DIMM");
    case 0x0A: return QStringLiteral("TSOP");
    case 0x0B: return QStringLiteral("Row of chips");
    case 0x0C: return QStringLiteral("RIMM");
    case 0x0D: return QStringLiteral("SODIMM");
    case 0x0E: return QStringLiteral("SRIMM");
    case 0x0F: return QStringLiteral("FB-DIMM");
    case 0x10: return QStringLiteral("Die");
    case 0x11: return QStringLiteral("CAMM");
    default: return {};  // 0x01 Other, 0x02 Unknown, anything newer
    }
}

QVector<SmbiosMemoryDevice> parseUdevMemoryDevices(const QString &text)
{
    // "E:MEMORY_DEVICE_3_CONFIGURED_SPEED_MTS=6000"
    static const QRegularExpression line(QStringLiteral(R"(^E:MEMORY_DEVICE_(\d+)_([A-Z_]+)=(.*)$)"),
                                         QRegularExpression::MultilineOption);
    std::map<int, SmbiosMemoryDevice> devices;
    std::map<int, bool> absent;  // PRESENT=0: an empty slot
    for (auto it = line.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const int n = m.captured(1).toInt();
        const QString field = m.captured(2);
        const QString value = cleanFirmwareString(m.captured(3));
        SmbiosMemoryDevice &d = devices[n];
        const auto number = [&] { return value.toLongLong(); };
        // The whitelist (P23): every other field, SERIAL_NUMBER and
        // ASSET_TAG among them, is never looked at.
        if (field == QLatin1StringView("PRESENT"))
            absent[n] = value == QLatin1StringView("0");
        else if (field == QLatin1StringView("SIZE"))
            d.sizeBytes = number();
        else if (field == QLatin1StringView("TYPE"))
            d.type = value;
        else if (field == QLatin1StringView("SPEED_MTS"))
            d.speedMts = int(number());
        else if (field == QLatin1StringView("CONFIGURED_SPEED_MTS"))
            d.configuredSpeedMts = int(number());
        else if (field == QLatin1StringView("RANK"))
            d.rank = int(number());
        else if (field == QLatin1StringView("DATA_WIDTH"))
            d.dataWidth = int(number());
        else if (field == QLatin1StringView("TOTAL_WIDTH"))
            d.totalWidth = int(number());
        else if (field == QLatin1StringView("FORM_FACTOR"))
            d.formFactor = value;
        else if (field == QLatin1StringView("LOCATOR"))
            d.locator = value;
        else if (field == QLatin1StringView("BANK_LOCATOR"))
            d.bankLocator = value;
        else if (field == QLatin1StringView("PART_NUMBER"))
            d.partNumber = value;
        else if (field == QLatin1StringView("MANUFACTURER"))
            d.manufacturer = value;
    }
    QVector<SmbiosMemoryDevice> out;
    for (const auto &[n, d] : devices) {
        if (!absent[n] && d.sizeBytes > 0)
            out.append(d);
    }
    return out;
}

std::optional<SmbiosVersion> parseSmbiosEntryPoint(const QByteArray &ep)
{
    if (ep.startsWith("_SM3_") && ep.size() >= 9)
        return SmbiosVersion{uchar(ep[7]), uchar(ep[8])};
    if (ep.startsWith("_SM_") && ep.size() >= 8)
        return SmbiosVersion{uchar(ep[6]), uchar(ep[7])};
    return std::nullopt;
}

SmbiosInfo parseSmbiosTable(const QByteArray &table, SmbiosVersion version)
{
    SmbiosInfo info;
    info.version = version;
    for (const Structure &s : split(table)) {
        switch (s.type()) {
        case 0:
            info.biosVendor = s.string(0x04);
            info.biosVersion = s.string(0x05);
            info.biosDate = s.string(0x08);
            break;
        case 1:
            info.maker = s.string(0x04);
            info.product = s.string(0x05);
            info.productVersion = s.string(0x06);
            // 0x07 serial number and 0x08 UUID: never read (P23).
            info.family = s.string(0x1A);
            break;
        case 17: {
            const SmbiosMemoryDevice d = memoryDevice(s, version);
            if (d.sizeBytes > 0)
                info.memoryDevices.append(d);
            break;
        }
        default:
            break;
        }
    }
    return info;
}

std::optional<SmbiosInfo> parseWindowsRawSmbios(const QByteArray &raw)
{
    if (raw.size() < 8)
        return std::nullopt;
    const auto *p = reinterpret_cast<const uchar *>(raw.constData());
    const SmbiosVersion v{p[1], p[2]};
    const quint32 length = qFromLittleEndian<quint32>(p + 4);
    if (length > quint32(raw.size() - 8))
        return std::nullopt;
    return parseSmbiosTable(raw.mid(8, length), v);
}

int estimateMemoryChannels(const QVector<SmbiosMemoryDevice> &modules)
{
    if (modules.isEmpty())
        return 0;
    // "CHANNEL A", "Channel-B", "DIMM_A1", "ChannelA-DIMM0", "P0 CHANNEL C".
    static const QRegularExpression channel(QStringLiteral(R"(CHANNEL[\s_-]*([A-Z0-9]+))"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression dimm(QStringLiteral(R"(DIMM[\s_-]*([A-Z])\d)"),
                                         QRegularExpression::CaseInsensitiveOption);
    QSet<QString> seen;
    for (const SmbiosMemoryDevice &m : modules) {
        const QString where = m.bankLocator + QLatin1Char(' ') + m.locator;
        auto match = channel.match(where);
        if (!match.hasMatch())
            match = dimm.match(where);
        if (!match.hasMatch())
            return std::min<int>(modules.size(), 2);
        seen.insert(match.captured(1).toUpper());
    }
    return int(seen.size());
}

} // namespace sb::sysinfo
