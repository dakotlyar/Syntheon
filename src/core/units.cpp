// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/units.h"

#include <QLocale>

#include <cmath>

namespace sb::Units {

const QStringList &canonical()
{
    static const QStringList units = {
        QStringLiteral("MB/s"),   QStringLiteral("GB/s"),  QStringLiteral("IOPS"),   QStringLiteral("ns"),
        QStringLiteral("µs"),     QStringLiteral("ms"),    QStringLiteral("s"),      QStringLiteral("min"),
        QStringLiteral("fps"),    QStringLiteral("Mpx/s"), QStringLiteral("Gpx/s"),  QStringLiteral("MOps/s"),
        QStringLiteral("MFLOPS"), QStringLiteral("tok/s"), QStringLiteral("inf/s"),  QStringLiteral("it/s"),
        QStringLiteral("W"),      QStringLiteral("Wh"),    QStringLiteral("%"),      QStringLiteral("MT/s"),
        QStringLiteral("points"), QString(),
    };
    return units;
}

bool isCanonical(const QString &unit)
{
    return canonical().contains(unit);
}

QString formatNumber(double value, const QString &unit)
{
    if (!std::isfinite(value))
        return QStringLiteral("—");

    QLocale locale(QLocale::English, QLocale::UnitedStates);
    if (unit == QLatin1StringView("MT/s"))
        locale.setNumberOptions(QLocale::OmitGroupSeparator);

    const auto decimalsFor = [](double a) {
        if (a == 0.0 || a >= 100.0)
            return 0;
        if (a >= 1.0)
            return 1;
        // Two significant figures: 0.18, 0.012, 0.0012.
        return 1 - static_cast<int>(std::floor(std::log10(a)));
    };
    int decimals = decimalsFor(std::fabs(value));
    // Rounding can carry into the next bucket (99.96 -> 100.0); format it
    // as that bucket would (100).
    const double scale = std::pow(10.0, decimals);
    decimals = decimalsFor(std::round(std::fabs(value) * scale) / scale);
    QString s = locale.toString(value, 'f', decimals);
    if (s == QLatin1StringView("-0"))
        s = QStringLiteral("0");
    return s;
}

QString format(double value, const QString &unit, bool withUnit)
{
    const QString number = formatNumber(value, unit);
    if (!withUnit || unit.isEmpty() || unit == QLatin1StringView("points"))
        return number;
    if (unit == QLatin1StringView("%"))
        return number + unit;
    return number + QLatin1Char(' ') + unit;
}

} // namespace sb::Units
