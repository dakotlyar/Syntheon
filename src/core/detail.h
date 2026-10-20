// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/enums.h"
#include "core/result.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <variant>

namespace sb {

// P6: a detail page is a declarative list of standard blocks, rendered by one
// generic QML renderer, plus a Custom escape hatch. Layouts are plain data so
// they can be unit-tested and old runs still render.

// A cell shows a stored metric (by key) or literal text.
struct DetailCell {
    QString metricKey;
    QString text;

    static DetailCell metric(const QString &key) { return {key, {}}; }
    static DetailCell literal(const QString &text) { return {{}, text}; }
    bool operator==(const DetailCell &) const = default;
};

struct KpiItem {
    QString label;
    DetailCell value;
    QString unitOverride;

    bool operator==(const KpiItem &) const = default;
};

struct KpisBlock {
    QVector<KpiItem> items;
    bool operator==(const KpisBlock &) const = default;
};

struct TableBlock {
    QStringList columns;
    QVector<QVector<DetailCell>> rows;
    // Never paired with a neighbouring table: always the page's full width.
    bool fullWidth = false;
    // Rows drawn in bold (totals: "CPU score", "Single-thread mark").
    QVector<int> boldRows;
    bool operator==(const TableBlock &) const = default;
};

// CrystalDiskMark-style grid (Storage).
struct GridCell {
    QString metricKey;
    QString secondaryMetricKey;
    bool operator==(const GridCell &) const = default;
};

struct GridBlock {
    QStringList rowLabels;
    QStringList rowSmallText;
    QStringList columnLabels;
    QVector<QVector<GridCell>> cells;  // [row][column]
    bool operator==(const GridBlock &) const = default;
};

struct ChartAxis {
    // Log10: a log axis like Log2 (the positions are the same); it differs
    // only in the ticks chosen when none are given (Phase 3 D13).
    enum class Scale { Linear, Log2, Log10 };
    Scale scale = Scale::Linear;
    QStringList tickLabels;
    // The x position of each label, in the series' x unit (KiB, minutes). If
    // shorter than tickLabels, the labels are spread evenly. (The y axis:
    // see LineChartBlock::yAxis.)
    QVector<double> tickValues;
    bool operator==(const ChartAxis &) const = default;
};

struct ChartAnnotation {
    double x = 0.0;
    QString label;
    bool operator==(const ChartAnnotation &) const = default;
};

struct LineChartBlock {
    QStringList seriesKeys;
    ChartAxis xAxis;
    QString yUnit;
    QVector<ChartAnnotation> annotations;
    // The y axis (Phase 3 D13). Linear, the default, is the chart as it
    // always was: five grid lines from 0 to a rounded maximum (tickLabels
    // and tickValues unused). Log2 / Log10: a log axis with a grid line at
    // each tickValue (labelled tickLabels[i], else the number), spanning the
    // ticks and the data; with no tickValues, powers of two (Log2) or a
    // 1-2-5 sequence (Log10) over the data.
    ChartAxis yAxis;
    // Open dots on the curve at these x values (the Phase 3 RAM mockup's
    // scored targets): the series' point at each x; an x the series has no
    // point at is skipped. Empty by default.
    QVector<double> markers;
    // Draw the points in x order instead of the order the series stores
    // them (Phase 3, the RAM test's loaded latency: stored in load-level
    // order, drawn by bandwidth). Off by default: every chart as before.
    bool sortByX = false;
    bool operator==(const LineChartBlock &) const = default;
};

struct NoteBlock {
    QString text;
    Tone tone = Tone::Neutral;
    bool operator==(const NoteBlock &) const = default;
};

struct NaStateBlock {
    QString title;
    QString body;
    bool operator==(const NaStateBlock &) const = default;
};

// Escape hatch: a QML type registered by the app, only when no block fits.
struct CustomBlock {
    QString qmlType;
    QVariantMap data;
    bool operator==(const CustomBlock &) const = default;
};

using DetailBlockContent =
    std::variant<KpisBlock, TableBlock, GridBlock, LineChartBlock, NoteBlock, NaStateBlock, CustomBlock>;

struct DetailBlock {
    QString title;  // optional box heading
    DetailBlockContent content;
    bool operator==(const DetailBlock &) const = default;
};

struct DetailLayout {
    QString subtitle;            // "Score 1,284 · AVX2 path · 16 threads"
    QStringList headlineMetrics; // ordered metric keys for the Results row
    QVector<DetailBlock> blocks;
    bool operator==(const DetailLayout &) const = default;
};

// A series that marks where windows start (key "<name>.windows", e.g. the
// CPU test's telemetry.windows: x = a start time, y = a code), not a
// measurement: no chart, no comparison, no export treats it as data.
bool isMarkerSeries(const Series &s);

// The fallback layout (ITest::detail()'s default): KPIs from the headline
// metrics, a table of every metric, and one chart per series (not the
// marker series).
DetailLayout genericDetailLayout(const TestResult &result);

// Every N/A test's Detail page (Dave, 2026-10-17): a result whose status is
// NotAvailable shows one NaStateBlock with the reason, not the test's normal
// layout (whose fields would all be dashes). A layout that already has its own
// NaStateBlock (NPU) is kept as the test made it. Other statuses: unchanged.
DetailLayout withNotAvailableState(DetailLayout layout, const TestResult &result, const QString &testName);

} // namespace sb
