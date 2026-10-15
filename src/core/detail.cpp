#include "core/detail.h"

#include "core/units.h"

namespace sb {

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
