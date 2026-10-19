// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/detail.h"
#include "core/result.h"

#include <QVariantList>

namespace sb {

// Turns a test's DetailLayout plus its stored TestResult into plain QML data
// (P6): every number is already formatted (Units::format), so the QML block
// renderers hold no logic and old runs render from what was stored.
//
// One entry per block, each a QVariantMap with "type" and "title":
//   kpis      items [{label, value, unit}]
//   table     columns [text], rows [[text]]
//   grid      columns [text], rows [{label, small, cells:[{main, sub}]}]
//   linechart points [{x,y}], logX, xTicks [{x,label}], yUnit, annotations
//             [{x,label}], colorToken, logY, yTicks [{y,label}] (log y
//             only), markers [{x,y}]
//   note      text, tone
//   nastate   body
//   custom    qmlType, data  (the escape hatch; unknown types show a note)
QVariantList buildDetailBlocks(const DetailLayout &layout, const TestResult &result, const QString &colorToken);

// A metric formatted for a cell: "7,012 MB/s", "AVX2", or "—" when missing.
QString formatMetric(const TestResult &result, const QString &key, bool withUnit = true);

// Compact count for secondary IOPS text: 181000 -> "181k IOPS".
QString formatCompactIops(double iops);

} // namespace sb
