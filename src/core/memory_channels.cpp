// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/memory_channels.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace sb {

namespace {

// What comes before the channel or DIMM part, when it names a socket or a
// memory controller ("P0", "CPU1", "Socket0", "Controller1", also with a
// space: "NODE 0", "CPU 1"; AP-5 round 4 L1), as "NODE0": other prefixes
// (board jumpers like "J1_") would split one channel in two.
QString socketPrefix(const QString &before)
{
    static const QRegularExpression socket(QStringLiteral(R"((?:^|[^A-Za-z])(CPU|P|SOCKET|NODE|CONTROLLER|MC|IMC)\s*(\d+))"),
                                           QRegularExpression::CaseInsensitiveOption);
    QString found;
    auto it = socket.globalMatch(before);
    while (it.hasNext()) {
        const auto m = it.next();
        found += (found.isEmpty() ? QString() : QStringLiteral(" ")) + m.captured(1).toUpper() + m.captured(2);
    }
    return found;
}

} // namespace

QString memoryChannelKey(const QString &bankLocator, const QString &locator)
{
    static const QRegularExpression channel(QStringLiteral(R"(CHANNEL[\s_-]*([A-Z0-9]+))"),
                                            QRegularExpression::CaseInsensitiveOption);
    // "DIMMA1", "DIMM_A1", and EPYC's "DIMM_P0_A0" (its letter after the
    // socket part).
    static const QRegularExpression dimm(QStringLiteral(R"(DIMM[\s_-]*(?:(P\d+)[\s_-]*)?([A-Z])\d)"),
                                         QRegularExpression::CaseInsensitiveOption);
    for (const QString &where : {bankLocator, locator}) {
        if (const auto m = channel.match(where); m.hasMatch())
            return (socketPrefix(where.left(m.capturedStart())) + QLatin1Char(' ') + m.captured(1)).trimmed().toUpper();
    }
    for (const QString &where : {bankLocator, locator}) {
        if (const auto m = dimm.match(where); m.hasMatch()) {
            // The socket can come before "DIMM" ("CPU1_DIMM_A1", "P2-DIMMA1")
            // or inside it ("DIMM_P1_A0"); in "DIMM_P1" the P is the slot's
            // letter, not a socket (AP-5 round 4 L2).
            QString socket = socketPrefix(where.left(m.capturedStart()));
            if (!m.captured(1).isEmpty())
                socket += QLatin1Char(' ') + m.captured(1).toUpper();
            return (socket + QLatin1Char(' ') + m.captured(2)).trimmed().toUpper();
        }
    }
    return {};
}

MemoryChannelCount countMemoryChannels(const QList<QPair<QString, QString>> &devices)
{
    MemoryChannelCount c;
    if (devices.isEmpty())
        return c;
    QSet<QString> seen;
    for (const auto &[bank, locator] : devices) {
        const QString key = memoryChannelKey(bank, locator);
        if (key.isEmpty()) {
            c.channels = std::min<int>(int(devices.size()), 2);
            return c;
        }
        seen.insert(key);
    }
    c.channels = int(seen.size());
    c.fromLocators = true;
    return c;
}

} // namespace sb
