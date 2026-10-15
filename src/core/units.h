// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

namespace sb::Units {

// The canonical unit strings (Proposed in §2.8). "" means unitless.
const QStringList &canonical();
bool isCanonical(const QString &unit);

// Central number formatting, matching the mockup:
//   |v| >= 100  -> no decimals, thousands separators   "7,012 MB/s"
//   |v| >= 1    -> one decimal                          "41.9 GB/s", "3.1 it/s"
//   |v| <  1    -> two significant figures              "0.18 s"
// "MT/s" is never grouped ("5600 MT/s"); "%" has no space ("98%").
// The format is fixed (en-US style), not locale-dependent, so runs read the
// same everywhere.
QString formatNumber(double value, const QString &unit = {});
QString format(double value, const QString &unit, bool withUnit = true);

} // namespace sb::Units
