// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/detail_blocks.h"

#include "core/units.h"

#include <QLocale>

#include <algorithm>
#include <cmath>

namespace sb {

QString formatMetric(const TestResult &result, const QString &key, bool withUnit)
{
    const Metric *m = result.metric(key);
    if (!m)
        return QStringLiteral("—");
    if (const auto *text = std::get_if<QString>(&m->value))
        return text->isEmpty() ? QStringLiteral("—") : *text;
    return Units::format(std::get<double>(m->value), m->unit, withUnit);
}

QString formatCompactIops(double iops)
{
    if (iops >= 1e6)
        return QStringLiteral("%1M IOPS").arg(iops / 1e6, 0, 'f', 1);
    if (iops >= 1e3)
        return QStringLiteral("%1k IOPS").arg(std::llround(iops / 1e3));
    return QStringLiteral("%1 IOPS").arg(std::llround(iops));
}

namespace {

QString cellText(const TestResult &r, const DetailCell &c)
{
    if (!c.metricKey.isEmpty())
        return formatMetric(r, c.metricKey);
    return c.text;
}

QVariantMap base(const QString &type, const QString &title)
{
    return {{QStringLiteral("type"), type}, {QStringLiteral("title"), title}};
}

QVariantMap kpis(const KpisBlock &b, const DetailBlock &block, const TestResult &r)
{
    QVariantMap out = base(QStringLiteral("kpis"), block.title);
    QVariantList items;
    for (const KpiItem &k : b.items) {
        QString value, unit = k.unitOverride;
        if (!k.value.metricKey.isEmpty()) {
            const Metric *m = r.metric(k.value.metricKey);
            value = formatMetric(r, k.value.metricKey, false);
            if (m && unit.isEmpty())
                unit = m->unit == QLatin1StringView("points") ? QStringLiteral("points") : m->unit;
        } else {
            value = k.value.text;
        }
        items.append(QVariantMap{{QStringLiteral("label"), k.label}, {QStringLiteral("value"), value},
                                 {QStringLiteral("unit"), unit}});
    }
    out.insert(QStringLiteral("items"), items);
    return out;
}

QVariantMap table(const TableBlock &b, const DetailBlock &block, const TestResult &r)
{
    QVariantMap out = base(QStringLiteral("table"), block.title);
    QVariantList rows;
    for (const auto &row : b.rows) {
        QStringList cells;
        for (const DetailCell &c : row)
            cells.append(cellText(r, c));
        rows.append(cells);
    }
    out.insert(QStringLiteral("columns"), b.columns);
    out.insert(QStringLiteral("rows"), rows);
    out.insert(QStringLiteral("fullWidth"), b.fullWidth);
    QVariantList bold;
    for (const int i : b.boldRows)
        bold.append(i);
    out.insert(QStringLiteral("boldRows"), bold);
    return out;
}

QVariantMap grid(const GridBlock &b, const DetailBlock &block, const TestResult &r)
{
    QVariantMap out = base(QStringLiteral("grid"), block.title);
    QVariantList rows;
    for (int i = 0; i < b.cells.size(); ++i) {
        QVariantList cells;
        for (const GridCell &c : b.cells[i]) {
            const Metric *m = r.metric(c.metricKey);
            QString sub = m ? m->unit : QString();
            if (!c.secondaryMetricKey.isEmpty()) {
                const Metric *s = r.metric(c.secondaryMetricKey);
                if (s && s->isNumeric())
                    sub += QStringLiteral(" · ") + formatCompactIops(std::get<double>(s->value));
            }
            cells.append(QVariantMap{{QStringLiteral("main"), formatMetric(r, c.metricKey, false)},
                                     {QStringLiteral("sub"), sub}});
        }
        rows.append(QVariantMap{{QStringLiteral("label"), b.rowLabels.value(i)},
                                {QStringLiteral("small"), b.rowSmallText.value(i)},
                                {QStringLiteral("cells"), cells}});
    }
    out.insert(QStringLiteral("columns"), b.columnLabels);
    out.insert(QStringLiteral("rows"), rows);
    return out;
}

// A log y axis's grid lines [{y, label}] (LineChartBlock::yAxis): the given
// ticks, or powers of two / a 1-2-5 sequence spanning the positive data.
// Empty for a linear axis, which the chart lays out itself.
QVariantList yTicks(const ChartAxis &axis, const QVariantList &points)
{
    QVariantList out;
    if (axis.scale == ChartAxis::Scale::Linear)
        return out;
    const auto tick = [&](double y, const QString &label) {
        out.append(QVariantMap{{QStringLiteral("y"), y}, {QStringLiteral("label"), label}});
    };
    if (!axis.tickValues.isEmpty()) {
        for (int i = 0; i < axis.tickValues.size(); ++i) {
            const double y = axis.tickValues[i];
            if (y > 0)
                tick(y, i < axis.tickLabels.size() ? axis.tickLabels[i] : QLocale(QLocale::English).toString(y, 'g', 6));
        }
        return out;
    }
    double lo = INFINITY, hi = -INFINITY;
    for (const QVariant &v : points) {
        const double y = v.toMap().value(QStringLiteral("y")).toDouble();
        if (y > 0) {
            lo = std::min(lo, y);
            hi = std::max(hi, y);
        }
    }
    // Non-finite or denormal data (AP-5 L7): no ticks rather than a loop
    // that never reaches hi (an infinite hi) or never moves (a start of 0).
    if (!std::isfinite(lo) || !std::isfinite(hi) || lo < 1e-300 || hi > 1e300)
        return out;
    // From the step at or below the lowest point to the one at or above the highest.
    QVector<double> steps;
    if (axis.scale == ChartAxis::Scale::Log2) {
        for (double y = std::exp2(std::floor(std::log2(lo))); y <= hi * 2; y *= 2)
            steps.append(y);
    } else {
        for (double decade = std::pow(10.0, std::floor(std::log10(lo))); decade <= hi * 10; decade *= 10) {
            for (const double m : {1.0, 2.0, 5.0})
                steps.append(m * decade);
        }
    }
    int first = 0, last = int(steps.size()) - 1;
    while (first + 1 < steps.size() && steps[first + 1] <= lo)
        ++first;
    while (last > first && steps[last - 1] >= hi)
        --last;
    for (int i = first; i <= last; ++i)
        tick(steps[i], QLocale(QLocale::English).toString(steps[i], 'g', 6));
    return out;
}

QVariantMap lineChart(const LineChartBlock &b, const DetailBlock &block, const TestResult &r, const QString &colorToken)
{
    QVariantMap out = base(QStringLiteral("linechart"), block.title);
    QVariantList points;
    double minX = INFINITY, maxX = -INFINITY;
    for (const QString &key : b.seriesKeys) {
        for (const Series &s : r.series) {
            if (s.key != key)
                continue;
            for (const QPointF &p : s.points) {
                points.append(QVariantMap{{QStringLiteral("x"), p.x()}, {QStringLiteral("y"), p.y()}});
                minX = std::min(minX, p.x());
                maxX = std::max(maxX, p.x());
            }
            break;  // one curve per block
        }
        if (!points.isEmpty())
            break;
    }
    if (b.sortByX)
        std::stable_sort(points.begin(), points.end(), [](const QVariant &a, const QVariant &c) {
            return a.toMap().value(QStringLiteral("x")).toDouble() < c.toMap().value(QStringLiteral("x")).toDouble();
        });
    QVariantList ticks;
    for (int i = 0; i < b.xAxis.tickLabels.size(); ++i) {
        double x;
        if (i < b.xAxis.tickValues.size())
            x = b.xAxis.tickValues[i];
        else if (b.xAxis.tickLabels.size() > 1 && std::isfinite(minX))
            x = minX + (maxX - minX) * i / (b.xAxis.tickLabels.size() - 1);  // spread evenly
        else
            x = std::isfinite(minX) ? minX : 0;
        ticks.append(QVariantMap{{QStringLiteral("x"), x}, {QStringLiteral("label"), b.xAxis.tickLabels[i]}});
    }
    QVariantList notes;
    for (const ChartAnnotation &a : b.annotations)
        notes.append(QVariantMap{{QStringLiteral("x"), a.x}, {QStringLiteral("label"), a.label}});
    // The markers: the curve's point at each marker x (within a relative
    // 1e-9, for values that went through the database).
    QVariantList markers;
    for (const double mx : b.markers) {
        for (const QVariant &v : std::as_const(points)) {
            const QVariantMap p = v.toMap();
            const double px = p.value(QStringLiteral("x")).toDouble();
            if (std::abs(px - mx) <= 1e-9 * std::max(std::abs(px), std::abs(mx))) {
                markers.append(p);
                break;
            }
        }
    }
    out.insert(QStringLiteral("points"), points);
    out.insert(QStringLiteral("logX"), b.xAxis.scale != ChartAxis::Scale::Linear);
    out.insert(QStringLiteral("logY"), b.yAxis.scale != ChartAxis::Scale::Linear);
    out.insert(QStringLiteral("yTicks"), yTicks(b.yAxis, points));
    out.insert(QStringLiteral("markers"), markers);
    out.insert(QStringLiteral("xTicks"), ticks);
    out.insert(QStringLiteral("yUnit"), b.yUnit);
    out.insert(QStringLiteral("annotations"), notes);
    out.insert(QStringLiteral("colorToken"), colorToken);
    return out;
}

} // namespace

QVariantList buildDetailBlocks(const DetailLayout &layout, const TestResult &result, const QString &colorToken)
{
    QVariantList out;
    for (const DetailBlock &block : layout.blocks) {
        if (const auto *b = std::get_if<KpisBlock>(&block.content)) {
            out.append(kpis(*b, block, result));
        } else if (const auto *b = std::get_if<TableBlock>(&block.content)) {
            out.append(table(*b, block, result));
        } else if (const auto *b = std::get_if<GridBlock>(&block.content)) {
            out.append(grid(*b, block, result));
        } else if (const auto *b = std::get_if<LineChartBlock>(&block.content)) {
            out.append(lineChart(*b, block, result, colorToken));
        } else if (const auto *b = std::get_if<NoteBlock>(&block.content)) {
            QVariantMap m = base(QStringLiteral("note"), block.title);
            m.insert(QStringLiteral("text"), b->text);
            m.insert(QStringLiteral("tone"), toKey(b->tone));
            out.append(m);
        } else if (const auto *b = std::get_if<NaStateBlock>(&block.content)) {
            QVariantMap m = base(QStringLiteral("nastate"), b->title);
            m.insert(QStringLiteral("body"), b->body);
            out.append(m);
        } else if (const auto *b = std::get_if<CustomBlock>(&block.content)) {
            QVariantMap m = base(QStringLiteral("custom"), block.title);
            m.insert(QStringLiteral("qmlType"), b->qmlType);
            m.insert(QStringLiteral("data"), b->data);
            out.append(m);
        }
    }
    return out;
}

} // namespace sb
