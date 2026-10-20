// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/config/readout.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>

#include <algorithm>
#include <cmath>
#include <map>

namespace sb::ram {

namespace {

const QString kDash = QStringLiteral("—");

Metric text(const char *key, const QString &label, const QString &value)
{
    return {QLatin1StringView(key), {}, label, value, {}, Direction::Neutral, false};
}

QString grouped(int value)
{
    return QLocale(QLocale::English).toString(value);
}

// "2R"; "1R/2R" when the modules differ; empty if any is unknown.
QString ranksText(const QVector<int> &ranks)
{
    QStringList distinct;
    for (const int r : ranks) {
        if (r <= 0)
            return {};
        const QString t = QStringLiteral("%1R").arg(r);
        if (!distinct.contains(t))
            distinct << t;
    }
    std::sort(distinct.begin(), distinct.end());
    return distinct.join(QLatin1Char('/'));
}

} // namespace

QString memorySizeText(double bytes)
{
    const double gib = bytes / (1024.0 * 1024 * 1024);
    if (gib >= 1) {
        const double whole = std::round(gib);
        return std::abs(gib - whole) < 0.05 ? QStringLiteral("%1 GB").arg(qint64(whole))
                                             : QStringLiteral("%1 GB").arg(gib, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(qint64(std::round(bytes / (1024.0 * 1024))));
}

QStringList moduleFields(const QString &value)
{
    QStringList f = value.split(QStringLiteral(" · "));
    while (f.size() < kModuleFields)
        f << kDash;
    return f.mid(0, kModuleFields);
}

MemoryReadout memoryReadout(const SystemSnapshot &system, bool windows)
{
    MemoryReadout out;
    const QJsonObject memory = system.data.value(QStringLiteral("memory")).toObject();
    const QJsonArray modules = memory.value(QStringLiteral("modules")).toArray();
    const QString source = memory.value(QStringLiteral("modules_source")).toString();
    if (modules.isEmpty()) {
        if (memory.value(QStringLiteral("modules_known")).toBool())
            out.reason = QStringLiteral("the firmware lists no memory devices");
        else if (windows)
            out.reason = QStringLiteral("the firmware's SMBIOS tables couldn't be read");
        else
            out.reason = QStringLiteral("the SMBIOS helper wasn't authorised and udev has no memory data");
        out.metrics.append(text("config.source", QStringLiteral("Memory details"), QStringLiteral("not available: ") + out.reason));
        out.summary = QStringLiteral("Memory details: not available (%1)").arg(out.reason);
        return out;
    }
    out.available = true;

    QStringList types;
    std::map<qint64, int> bySize;  // size -> count, for "2 × 16 GB"
    QVector<int> ranks;
    double total = 0;
    int width = 0;
    bool widthKnown = true;
    int configured = 0, rated = 0;  // the lowest of each (0: none reported)
    QStringList lines;
    for (const QJsonValue &v : modules) {
        const QJsonObject m = v.toObject();
        const auto size = qint64(m.value(QStringLiteral("size_bytes")).toDouble());
        const QString type = m.value(QStringLiteral("type")).toString();
        const int speed = m.value(QStringLiteral("speed_mts")).toInt();
        const int conf = m.value(QStringLiteral("configured_speed_mts")).toInt();
        const int rank = m.value(QStringLiteral("rank")).toInt();
        const int dataWidth = m.value(QStringLiteral("data_width")).toInt();
        total += double(size);
        ++bySize[size];
        if (!type.isEmpty() && !types.contains(type))
            types << type;
        ranks << rank;
        if (dataWidth > 0)
            width += dataWidth;
        else
            widthKnown = false;
        const int effective = conf > 0 ? conf : speed;
        if (effective > 0)
            configured = configured == 0 ? effective : std::min(configured, effective);
        if (speed > 0)
            rated = rated == 0 ? speed : std::min(rated, speed);

        // slot · size · type · speed · ranks · maker · part number
        const QString locator = m.value(QStringLiteral("locator")).toString();
        const QString bank = m.value(QStringLiteral("bank_locator")).toString();
        QString slot = locator.isEmpty() ? bank : locator;
        if (!locator.isEmpty() && !bank.isEmpty())
            slot = QStringLiteral("%1 (%2)").arg(locator, bank);
        const auto field = [](const QString &s) { return s.isEmpty() ? kDash : s; };
        lines << QStringList{field(slot), size > 0 ? memorySizeText(double(size)) : kDash, field(type),
                             effective > 0 ? QStringLiteral("%1 MT/s").arg(grouped(effective)) : kDash,
                             rank > 0 ? QStringLiteral("%1R").arg(rank) : kDash,
                             field(m.value(QStringLiteral("manufacturer")).toString()),
                             field(m.value(QStringLiteral("part_number")).toString())}
                     .join(QStringLiteral(" · "));
    }

    const QString typeText = types.join(QLatin1Char('/'));
    if (configured > 0)
        out.metrics.append({QStringLiteral("speed"), {}, QStringLiteral("Configured speed"), double(configured),
                            QStringLiteral("MT/s"), Direction::Neutral, false});
    const int channels = memory.value(QStringLiteral("channels")).toInt();
    if (channels > 0)
        out.metrics.append({QStringLiteral("channels"), {}, QStringLiteral("Channels (estimate)"), double(channels), QString(),
                            Direction::Neutral, false});
    if (!typeText.isEmpty())
        out.metrics.append(text("config.type", QStringLiteral("Memory type"), typeText));
    out.metrics.append(text("config.total", QStringLiteral("Installed memory"), memorySizeText(total)));
    QStringList groups;
    for (auto it = bySize.rbegin(); it != bySize.rend(); ++it)
        groups << QStringLiteral("%1 × %2").arg(it->second).arg(memorySizeText(double(it->first)));
    out.metrics.append(text("config.modules", QStringLiteral("Modules"), groups.join(QStringLiteral(" + "))));
    if (const QString r = ranksText(ranks); !r.isEmpty())
        out.metrics.append(text("config.ranks", QStringLiteral("Ranks"), r));
    if (widthKnown && width > 0)
        out.metrics.append(text("config.width", QStringLiteral("Bus width"), QStringLiteral("%1-bit").arg(width)));
    if (rated > 0)
        out.metrics.append(text("config.rated", QStringLiteral("Rated speed"), QStringLiteral("%1 MT/s").arg(grouped(rated))));
    out.metrics.append(text("config.source", QStringLiteral("Memory details"),
                            source == QLatin1StringView("udev") ? QStringLiteral("SMBIOS (udev)")
                                                                : QStringLiteral("SMBIOS (System info)")));
    if (widthKnown && width > 0 && configured > 0) {
        out.peakGBs = double(configured) * double(width) / 8.0 / 1000.0;
        out.metrics.append({QStringLiteral("config.peak"), {}, QStringLiteral("Theoretical peak"), *out.peakGBs,
                            QStringLiteral("GB/s"), Direction::Neutral, false});
    }
    for (int i = 0; i < lines.size() && i < 16; ++i)
        out.metrics.append({QStringLiteral("config.module.%1").arg(i + 1), {}, QStringLiteral("Module %1").arg(i + 1), lines[i],
                            {}, Direction::Neutral, false});

    QStringList summary;
    if (!typeText.isEmpty())
        summary << (configured > 0 ? QStringLiteral("%1-%2").arg(typeText).arg(configured) : typeText);
    summary << groups.join(QStringLiteral(" + "));
    if (channels > 0)
        summary << (channels == 1 ? QStringLiteral("1 channel") : QStringLiteral("%1 channels").arg(channels));
    out.summary = summary.join(QStringLiteral(" · ")) + QStringLiteral(" (SMBIOS)");
    return out;
}

} // namespace sb::ram
