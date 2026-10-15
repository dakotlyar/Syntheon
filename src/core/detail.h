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
    enum class Scale { Linear, Log2 };
    Scale scale = Scale::Linear;
    QStringList tickLabels;
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

// The fallback layout (ITest::detail()'s default): KPIs from the headline
// metrics, a table of every metric, and one chart per series.
DetailLayout genericDetailLayout(const TestResult &result);

} // namespace sb
