// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/memory_channels.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace sb {

QString memoryChannelKey(const QString &bankLocator, const QString &locator)
{
    static const QRegularExpression channel(QStringLiteral(R"(CHANNEL[\s_-]*([A-Z0-9]+))"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression dimm(QStringLiteral(R"(DIMM[\s_-]*([A-Z])\d)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression trailing(QStringLiteral(R"([^A-Za-z0-9]+$)"));
    for (const QString &where : {bankLocator, locator}) {
        if (const auto m = channel.match(where); m.hasMatch()) {
            // What comes before "CHANNEL" tells sockets / controllers apart.
            QString prefix = where.left(m.capturedStart()).trimmed();
            prefix.remove(trailing);
            return (prefix + QLatin1Char(' ') + m.captured(1)).trimmed().toUpper();
        }
    }
    for (const QString &where : {bankLocator, locator}) {
        if (const auto m = dimm.match(where); m.hasMatch()) {
            // ...and before "DIMM" too ("CPU1_DIMM_A1", "P2-DIMMA1"; AP-5).
            QString prefix = where.left(m.capturedStart()).trimmed();
            prefix.remove(trailing);
            return (prefix + QLatin1Char(' ') + m.captured(1)).trimmed().toUpper();
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
