// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/parsers/jep106.h"

#include <QRegularExpression>

namespace sb::sysinfo {

namespace {

struct Entry {
    int bank;
    int code;  // as JEDEC lists it, with the odd-parity bit
    const char *name;
};

// From i2c-tools' decode-dimms JEP106 table (bank, code with parity, its
// name), shortened to the brand people know.
constexpr Entry kMakers[] = {
    {1, 0x2C, "Micron"},          // Micron Technology
    {1, 0x54, "HP"},              // Hewlett-Packard
    {1, 0xAD, "SK hynix"},        // SK Hynix (former Hyundai Electronics)
    {1, 0xCE, "Samsung"},
    {2, 0x32, "Mushkin"},         // Mushkin Enhanced Memory
    {2, 0x4F, "Transcend"},       // Transcend Information
    {2, 0x7A, "Apacer"},          // Apacer Technology
    {2, 0x94, "Smart Modular"},
    {2, 0x98, "Kingston"},
    {2, 0xBA, "PNY"},             // PNY Technologies Inc.
    {3, 0x9E, "Corsair"},
    {3, 0xFE, "Elpida"},
    {4, 0x0B, "Nanya"},           // Nanya Technology
    {4, 0x25, "Kingmax"},         // Kingmax Semiconductor
    {4, 0x94, "Mushkin"},
    {5, 0x43, "Ramaxel"},         // Ramaxel Technology
    {5, 0xCB, "ADATA"},           // A-DATA Technology
    {5, 0xCD, "G.Skill"},         // G Skill Intl
    {5, 0xEF, "Team Group"},      // Team Group Inc.
    {6, 0x02, "Patriot"},         // Patriot Memory
    {6, 0x51, "Qimonda"},
    {6, 0x9B, "Crucial"},         // Crucial Technology
    {7, 0x5E, "Lexar"},           // Lexar Media
    {7, 0x6D, "V-Color"},         // V-Color Technology Inc.
    {7, 0xD3, "Silicon Power"},   // Silicon Power Computer & Communications
    {7, 0xF1, "Innodisk"},        // InnoDisk Corporation
    {8, 0x5D, "Goodram"},         // Wilk Elektronik S.A.
    {8, 0xEA, "Transcend"},       // Transcend Technology Co. Ltd.
    {9, 0x98, "Klevv (Essencore)"},  // Essencore Limited
    {9, 0xC8, "Lenovo"},
    {9, 0xD9, "EVGA"},
    {10, 0x68, "Kimtigo"},        // Kimtigo Semiconductor (HK) Limited
    {10, 0xCD, "Longsys"},        // Shenzhen Longsys Electronics Co Ltd
    {10, 0xF7, "Netac"},          // Netac Technology Co Ltd
    {11, 0x31, "Biwin"},          // Biwin Semiconductor (HK) Co Ltd
    {11, 0x6B, "Acer"},
    {11, 0x76, "Lexar"},          // Lexar Co Limited
    {11, 0x91, "CXMT"},           // CXMT (former Innotron Memory Co Ltd)
    {11, 0xC2, "Thermaltake"},    // Thermaltake Technology Co Ltd
    {11, 0xD6, "Longsys"},        // Shenzhen Longsys Electronics Co Ltd
    {12, 0x92, "Kingbank"},       // Kingbank Technology Co Ltd
    {12, 0xDC, "ASUS"},
};

} // namespace

QString jep106Name(int bank, int code)
{
    for (const Entry &e : kMakers) {
        if (e.bank == bank && (e.code & 0x7F) == (code & 0x7F))
            return QString::fromLatin1(e.name);
    }
    return {};
}

bool isPlaceholderManufacturer(const QString &s)
{
    static const QRegularExpression placeholder(
        QStringLiteral("^(|unknown|not specified|undefined|to be filled by o\\.e\\.m\\.|manufacturer ?\\d*|"
                       "(0x)?[0-9a-f]{4,})$"),
        QRegularExpression::CaseInsensitiveOption);
    return placeholder.match(s.trimmed()).hasMatch();
}

ModuleManufacturer resolveModuleManufacturer(const QString &reported, int bank, int code)
{
    const QString r = reported.trimmed();
    if (!isPlaceholderManufacturer(r))
        return {r, QStringLiteral("reported")};
    if (bank > 0 && (code & 0x7F) != 0) {
        const QString name = jep106Name(bank, code);
        if (name.isEmpty()) {
            const QString hex = QString::number(code & 0xFF, 16).toUpper().rightJustified(2, QLatin1Char('0'));
            return {QStringLiteral("JEDEC bank %1, 0x%2").arg(bank).arg(hex), QStringLiteral("jedec")};
        }
        return {name, QStringLiteral("jedec")};
    }
    return {r, r.isEmpty() ? QString() : QStringLiteral("reported")};
}

} // namespace sb::sysinfo
