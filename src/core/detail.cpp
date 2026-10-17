// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/detail.h"

#include "core/units.h"

namespace sb {

DetailLayout withNotAvailableState(DetailLayout layout, const TestResult &result, const QString &testName)
{
    if (result.status != Status::NotAvailable)
        return layout;
    for (const DetailBlock &b : std::as_const(layout.blocks)) {
        if (std::holds_alternative<NaStateBlock>(b.content))
            return layout;
    }
    DetailLayout na;
    na.subtitle = QStringLiteral("Not available on this machine");
    na.headlineMetrics = layout.headlineMetrics;
    const QString title = testName.isEmpty() ? QStringLiteral("This test isn't available here")
                                             : QStringLiteral("%1 isn't available here").arg(testName);
    const QString body = result.statusReason.isEmpty() ? QStringLiteral("The test reported that it can't run on this machine.")
                                                       : result.statusReason;
    na.blocks.append({{}, NaStateBlock{title, body}});
    return na;
}

DetailLayout genericDetailLayout(const TestResult &result)
{
    DetailLayout layout;
    if (result.score)
        layout.subtitle = QStringLiteral("Score %1").arg(Units::format(*result.score, QStringLiteral("points"), false));

    KpisBlock kpis;
    for (const Metric &m : result.metrics) {
        if (!m.headline)
            continue;
        layout.headlineMetrics.append(m.key);
        kpis.items.append({m.label, DetailCell::metric(m.key), {}});
    }
    if (!kpis.items.isEmpty())
        layout.blocks.append({{}, kpis});

    if (!result.metrics.isEmpty()) {
        TableBlock table;
        table.columns = {QStringLiteral("Metric"), QStringLiteral("Value")};
        for (const Metric &m : result.metrics)
            table.rows.append({DetailCell::literal(m.label), DetailCell::metric(m.key)});
        layout.blocks.append({QStringLiteral("All metrics"), table});
    }

    for (const Series &s : result.series) {
        LineChartBlock chart;
        chart.seriesKeys = {s.key};
        chart.yUnit = s.yUnit;
        layout.blocks.append({s.label, chart});
    }

    if (layout.blocks.isEmpty() && !result.statusReason.isEmpty())
        layout.blocks.append({{}, NoteBlock{result.statusReason, Tone::Na}});

    return layout;
}

} // namespace sb
