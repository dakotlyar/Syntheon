// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/parsers/text_parsers.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace sb::sysinfo {

QMap<QString, QString> parseOsRelease(const QString &text)
{
    QMap<QString, QString> out;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        QString value = line.mid(eq + 1).trimmed();
        if (value.size() >= 2 && (value.front() == QLatin1Char('"') || value.front() == QLatin1Char('\''))
            && value.back() == value.front()) {
            value = value.mid(1, value.size() - 2);
            // Shell escapes allowed by os-release(5) inside double quotes.
            static const QRegularExpression esc(QStringLiteral(R"(\\([\\$"`]))"));
            value.replace(esc, QStringLiteral("\\1"));
        }
        out.insert(line.left(eq).trimmed(), value);
    }
    return out;
}

qint64 parseMemTotal(const QString &text)
{
    static const QRegularExpression re(QStringLiteral(R"(^MemTotal:\s+(\d+)\s*kB)"),
                                       QRegularExpression::MultilineOption);
    const auto m = re.match(text);
    return m.hasMatch() ? m.captured(1).toLongLong() * 1024 : 0;
}

namespace {

int parseIntAnyBase(const QString &s)
{
    bool ok = false;
    const int v = s.trimmed().toInt(&ok, 0);  // "0x41" or "8"
    return ok ? v : -1;
}

} // namespace

CpuInfoText parseCpuInfo(const QString &text)
{
    CpuInfoText out;
    ArmCoreId core;
    bool inProcessor = false;
    const auto flushCore = [&] {
        if (core.implementer >= 0 && core.part >= 0)
            out.armCores.append(core);
        core = {};
    };
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const qsizetype colon = raw.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            if (raw.trimmed().isEmpty() && inProcessor) {
                flushCore();
                inProcessor = false;
            }
            continue;
        }
        const QString key = raw.left(colon).trimmed();
        const QString value = raw.mid(colon + 1).trimmed();
        if (key == QLatin1StringView("processor")) {
            if (inProcessor)
                flushCore();
            inProcessor = true;
            ++out.logicalCpus;
        } else if (key == QLatin1StringView("vendor_id") && out.vendorId.isEmpty()) {
            out.vendorId = value;
        } else if (key == QLatin1StringView("model name") && out.modelName.isEmpty()) {
            out.modelName = value;
        } else if ((key == QLatin1StringView("flags") || key == QLatin1StringView("Features")) && out.flags.isEmpty()) {
            out.flags = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        } else if (key == QLatin1StringView("CPU implementer")) {
            core.implementer = parseIntAnyBase(value);
        } else if (key == QLatin1StringView("CPU variant")) {
            core.variant = parseIntAnyBase(value);
        } else if (key == QLatin1StringView("CPU part")) {
            core.part = parseIntAnyBase(value);
        } else if (key == QLatin1StringView("CPU revision")) {
            core.revision = parseIntAnyBase(value);
        } else if (key == QLatin1StringView("Hardware")) {
            out.hardware = value;
        }
    }
    if (inProcessor)
        flushCore();
    return out;
}

std::optional<ArmCoreId> parseMidr(const QString &text)
{
    bool ok = false;
    const quint64 v = text.trimmed().toULongLong(&ok, 16);
    if (!ok)
        return std::nullopt;
    ArmCoreId id;
    id.implementer = int((v >> 24) & 0xff);
    id.variant = int((v >> 20) & 0xf);
    id.part = int((v >> 4) & 0xfff);
    id.revision = int(v & 0xf);
    return id;
}

QString armVendorName(int implementer)
{
    switch (implementer) {
    case 0x41: return QStringLiteral("ARM");
    case 0x42: return QStringLiteral("Broadcom");
    case 0x46: return QStringLiteral("Fujitsu");
    case 0x48: return QStringLiteral("HiSilicon");
    case 0x4e: return QStringLiteral("NVIDIA");
    case 0x51: return QStringLiteral("Qualcomm");
    case 0x53: return QStringLiteral("Samsung");
    case 0x61: return QStringLiteral("Apple");
    case 0x6d: return QStringLiteral("Microsoft");
    case 0xc0: return QStringLiteral("Ampere");
    default: return QStringLiteral("implementer 0x%1").arg(implementer, 2, 16, QLatin1Char('0'));
    }
}

QString armCoreName(int implementer, int part)
{
    static const QHash<int, const char *> arm = {
        {0xd03, "Cortex-A53"},  {0xd04, "Cortex-A35"},   {0xd05, "Cortex-A55"},   {0xd07, "Cortex-A57"},
        {0xd08, "Cortex-A72"},  {0xd09, "Cortex-A73"},   {0xd0a, "Cortex-A75"},   {0xd0b, "Cortex-A76"},
        {0xd0c, "Neoverse-N1"}, {0xd0d, "Cortex-A77"},   {0xd0e, "Cortex-A76AE"}, {0xd40, "Neoverse-V1"},
        {0xd41, "Cortex-A78"},  {0xd42, "Cortex-A78AE"}, {0xd44, "Cortex-X1"},    {0xd46, "Cortex-A510"},
        {0xd47, "Cortex-A710"}, {0xd48, "Cortex-X2"},    {0xd49, "Neoverse-N2"},  {0xd4b, "Cortex-A78C"},
        {0xd4c, "Cortex-X1C"},  {0xd4d, "Cortex-A715"},  {0xd4e, "Cortex-X3"},    {0xd4f, "Neoverse-V2"},
        {0xd80, "Cortex-A520"}, {0xd81, "Cortex-A720"},  {0xd82, "Cortex-X4"},    {0xd84, "Neoverse-V3"},
        {0xd85, "Cortex-X925"}, {0xd87, "Cortex-A725"},  {0xd8e, "Neoverse-N3"},
    };
    static const QHash<int, const char *> qualcomm = {
        {0x001, "Oryon"},        {0x800, "Kryo 2xx Gold"}, {0x801, "Kryo 2xx Silver"}, {0x802, "Kryo 3xx Gold"},
        {0x803, "Kryo 3xx Silver"}, {0x804, "Kryo 4xx Gold"}, {0x805, "Kryo 4xx Silver"},
    };
    static const QHash<int, const char *> apple = {
        {0x022, "M1 Icestorm"}, {0x023, "M1 Firestorm"}, {0x032, "M2 Blizzard"}, {0x033, "M2 Avalanche"},
    };
    static const QHash<int, const char *> other = {
        {0x46001, "A64FX"}, {0x48d01, "TaiShan v110"}, {0x4e004, "Carmel"}, {0xc0ac3, "AmpereOne"},
    };
    const char *name = nullptr;
    if (implementer == 0x41)
        name = arm.value(part, nullptr);
    else if (implementer == 0x51)
        name = qualcomm.value(part, nullptr);
    else if (implementer == 0x61)
        name = apple.value(part, nullptr);
    else
        name = other.value((implementer << 12) | part, nullptr);
    if (name)
        return QString::fromLatin1(name);
    return QStringLiteral("%1 part 0x%2").arg(armVendorName(implementer)).arg(part, 3, 16, QLatin1Char('0'));
}

const QStringList &x86FeatureOrder()
{
    static const QStringList order = {
        QStringLiteral("sse2"),     QStringLiteral("sse3"),     QStringLiteral("ssse3"),      QStringLiteral("sse4.1"),
        QStringLiteral("sse4.2"),   QStringLiteral("popcnt"),   QStringLiteral("aes"),        QStringLiteral("f16c"),
        QStringLiteral("fma"),      QStringLiteral("bmi1"),     QStringLiteral("bmi2"),       QStringLiteral("avx"),
        QStringLiteral("avx2"),     QStringLiteral("sha"),      QStringLiteral("avx512f"),    QStringLiteral("avx512dq"),
        QStringLiteral("avx512cd"), QStringLiteral("avx512bw"), QStringLiteral("avx512vl"),   QStringLiteral("avx512vnni"),
        QStringLiteral("avx512bf16"), QStringLiteral("avxvnni"), QStringLiteral("amx"),
    };
    return order;
}

const QStringList &armFeatureOrder()
{
    static const QStringList order = {
        QStringLiteral("neon"), QStringLiteral("fp16"), QStringLiteral("dotprod"), QStringLiteral("i8mm"),
        QStringLiteral("bf16"), QStringLiteral("sve"),  QStringLiteral("sve2"),    QStringLiteral("sme"),
        QStringLiteral("crc32"), QStringLiteral("aes"), QStringLiteral("sha2"),    QStringLiteral("lse"),
    };
    return order;
}

namespace {

QStringList normalize(const QStringList &raw, const QHash<QString, QString> &map, const QStringList &order)
{
    QSet<QString> have;
    for (const QString &f : raw) {
        const auto it = map.constFind(f);
        if (it != map.cend())
            have.insert(*it);
    }
    QStringList out;
    for (const QString &f : order) {
        if (have.contains(f))
            out.append(f);
    }
    return out;
}

} // namespace

QStringList normalizeX86Flags(const QStringList &flags)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("sse2"), QStringLiteral("sse2")},         {QStringLiteral("pni"), QStringLiteral("sse3")},
        {QStringLiteral("ssse3"), QStringLiteral("ssse3")},       {QStringLiteral("sse4_1"), QStringLiteral("sse4.1")},
        {QStringLiteral("sse4_2"), QStringLiteral("sse4.2")},     {QStringLiteral("popcnt"), QStringLiteral("popcnt")},
        {QStringLiteral("aes"), QStringLiteral("aes")},           {QStringLiteral("f16c"), QStringLiteral("f16c")},
        {QStringLiteral("fma"), QStringLiteral("fma")},           {QStringLiteral("bmi1"), QStringLiteral("bmi1")},
        {QStringLiteral("bmi2"), QStringLiteral("bmi2")},         {QStringLiteral("avx"), QStringLiteral("avx")},
        {QStringLiteral("avx2"), QStringLiteral("avx2")},         {QStringLiteral("sha_ni"), QStringLiteral("sha")},
        {QStringLiteral("avx512f"), QStringLiteral("avx512f")},   {QStringLiteral("avx512dq"), QStringLiteral("avx512dq")},
        {QStringLiteral("avx512cd"), QStringLiteral("avx512cd")}, {QStringLiteral("avx512bw"), QStringLiteral("avx512bw")},
        {QStringLiteral("avx512vl"), QStringLiteral("avx512vl")}, {QStringLiteral("avx512_vnni"), QStringLiteral("avx512vnni")},
        {QStringLiteral("avx512_bf16"), QStringLiteral("avx512bf16")}, {QStringLiteral("avx_vnni"), QStringLiteral("avxvnni")},
        {QStringLiteral("amx_tile"), QStringLiteral("amx")},
    };
    return normalize(flags, map, x86FeatureOrder());
}

QStringList normalizeArmFeatures(const QStringList &features)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("asimd"), QStringLiteral("neon")},     {QStringLiteral("asimdhp"), QStringLiteral("fp16")},
        {QStringLiteral("fphp"), QStringLiteral("fp16")},      {QStringLiteral("asimddp"), QStringLiteral("dotprod")},
        {QStringLiteral("i8mm"), QStringLiteral("i8mm")},      {QStringLiteral("bf16"), QStringLiteral("bf16")},
        {QStringLiteral("sve"), QStringLiteral("sve")},        {QStringLiteral("sve2"), QStringLiteral("sve2")},
        {QStringLiteral("sme"), QStringLiteral("sme")},        {QStringLiteral("crc32"), QStringLiteral("crc32")},
        {QStringLiteral("aes"), QStringLiteral("aes")},        {QStringLiteral("sha2"), QStringLiteral("sha2")},
        {QStringLiteral("atomics"), QStringLiteral("lse")},
    };
    return normalize(features, map, armFeatureOrder());
}

QVector<int> parseCpuList(const QString &text)
{
    QVector<int> out;
    for (const QString &part : text.trimmed().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList range = part.split(QLatin1Char('-'));
        bool ok1 = false, ok2 = true;
        const int a = range.value(0).toInt(&ok1);
        const int b = range.size() > 1 ? range.value(1).toInt(&ok2) : a;
        if (!ok1 || !ok2 || b < a || b - a > 65536)
            continue;
        for (int i = a; i <= b; ++i)
            out.append(i);
    }
    return out;
}

qint64 parseCacheSize(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return 0;
    qint64 mult = 1;
    QString digits = t;
    const QChar unit = t.back().toUpper();
    if (unit == QLatin1Char('K'))
        mult = 1024;
    else if (unit == QLatin1Char('M'))
        mult = 1024 * 1024;
    else if (unit == QLatin1Char('G'))
        mult = qint64(1024) * 1024 * 1024;
    if (mult != 1)
        digits.chop(1);
    bool ok = false;
    const qint64 v = digits.toLongLong(&ok);
    return ok ? v * mult : 0;
}

QString cleanFirmwareString(const QString &s)
{
    const QString t = s.trimmed();
    static const QStringList placeholders = {
        QStringLiteral("To Be Filled By O.E.M."), QStringLiteral("To be filled by O.E.M."),
        QStringLiteral("Not Specified"),          QStringLiteral("Not Available"),
        QStringLiteral("Default string"),         QStringLiteral("System Product Name"),
        QStringLiteral("System manufacturer"),    QStringLiteral("Unknown"),
        QStringLiteral("None"),                   QStringLiteral("N/A"),
        QStringLiteral("O.E.M."),                 QStringLiteral("Undefined"),
    };
    for (const QString &p : placeholders) {
        if (t.compare(p, Qt::CaseInsensitive) == 0)
            return {};
    }
    return t;
}

QString pciVendorName(quint32 vendorId)
{
    switch (vendorId) {
    case 0x1002: case 0x1022: return QStringLiteral("AMD");
    case 0x10de: return QStringLiteral("NVIDIA");
    case 0x8086: return QStringLiteral("Intel");
    case 0x5143: case 0x17cb: return QStringLiteral("Qualcomm");
    case 0x13b5: return QStringLiteral("ARM");
    case 0x1414: return QStringLiteral("Microsoft");
    case 0x106b: return QStringLiteral("Apple");
    case 0x1af4: return QStringLiteral("Red Hat (virtio)");
    case 0x15ad: return QStringLiteral("VMware");
    case 0x80ee: return QStringLiteral("VirtualBox");
    default: return {};
    }
}

} // namespace sb::sysinfo
