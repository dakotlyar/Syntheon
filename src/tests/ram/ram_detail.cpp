// SPDX-License-Identifier: GPL-3.0-or-later
// The RAM test's Detail page (Phase 3 design §12, task 12): the layout of
// the approved mockup ([[Phase 3 - RAM Mockup]], Dave 2026-10-19), which wins
// where it differs from §12 (the charts share their rows with their tables
// 3:2, open dots on the scored targets, the memory configuration split into
// the SMBIOS readout and the derived peak, the two-line Memory KPI, both
// bandwidth lines in the Sub-tests row, amber neutral notes), plus the
// approved extras since: the Modules table's maker column (JEP106, d6f1fff)
// and P9's cap-hit note. Built only from the existing blocks, for Ok (with
// and without an L3), Partial, Stopped and small-page results. Pure over the
// stored result.
#include "core/units.h"
#include "tests/ram/config/readout.h"
#include "tests/ram/loaded/loaded_latency.h"
#include "tests/ram/ram_test.h"

#include <QLocale>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <optional>

namespace sb::tests {

namespace {

const QString kDash = QStringLiteral("—");

std::optional<double> number(const TestResult &r, const QString &key)
{
    const Metric *m = r.metric(key);
    if (!m || !m->isNumeric())
        return std::nullopt;
    return std::get<double>(m->value);
}

QString text(const TestResult &r, const QString &key)
{
    const Metric *m = r.metric(key);
    if (!m || m->isNumeric())
        return {};
    return std::get<QString>(m->value);
}

// The mockup's numbers: latency "0.8", "74.2", "138"; GB/s "72.4".
QString ns(double v)
{
    return v >= 100 ? QString::number(std::llround(v)) : QString::number(v, 'f', 1);
}

QString gbs(double v)
{
    return v == 0 ? QStringLiteral("0") : QString::number(v, 'f', 1);
}

QString nsOf(const TestResult &r, const QString &key, bool unit = false)
{
    const auto v = number(r, key);
    return v ? ns(*v) + (unit ? QStringLiteral(" ns") : QString()) : kDash;
}

QString gbsOf(const TestResult &r, const QString &key)
{
    const auto v = number(r, key);
    return v ? gbs(*v) : kDash;
}

const SubResult *sub(const TestResult &r, const char *id)
{
    for (const SubResult &s : r.subResults) {
        if (s.id == QLatin1StringView(id))
            return &s;
    }
    return nullptr;
}

QString statusText(const SubResult *s)
{
    if (!s)
        return QStringLiteral("Not run");
    switch (s->status) {
    case Status::Error: return QStringLiteral("Error");
    case Status::NotAvailable: return QStringLiteral("N/A");
    case Status::Cancelled: return QStringLiteral("Stopped");
    case Status::TimedOut: return QStringLiteral("Timed out");
    case Status::Interrupted: return QStringLiteral("Interrupted");
    case Status::NotRun: return QStringLiteral("Not run");
    case Status::Ok:
    case Status::Partial: break;
    }
    return kDash;
}

QString subScore(const SubResult *s)
{
    return s && s->score ? Units::format(*s->score, QStringLiteral("points"), false) : kDash;
}

const Series *series(const TestResult &r, const char *key)
{
    for (const Series &s : r.series) {
        if (s.key == QLatin1StringView(key))
            return &s;
    }
    return nullptr;
}

// "CPU 9" from "CPU 9 (top-ranked core, …)"; "unpinned" from "unpinned (…)".
QString coreOf(const QString &where)
{
    return where.section(QStringLiteral(" ("), 0, 0);
}

// "24 KiB", "5.7 MiB", "1 GiB" -> KiB.
std::optional<double> kibOf(const QString &size)
{
    const QStringList p = size.trimmed().split(QLatin1Char(' '));
    if (p.size() != 2)
        return std::nullopt;
    bool ok = false;
    const double v = p[0].toDouble(&ok);
    if (!ok)
        return std::nullopt;
    if (p[1] == QLatin1StringView("KiB"))
        return v;
    if (p[1] == QLatin1StringView("MiB"))
        return v * 1024;
    if (p[1] == QLatin1StringView("GiB"))
        return v * 1024 * 1024;
    return std::nullopt;
}

// One level of cache.levels ("L1d 48 KiB", "L3 32 MiB (shared by CPUs …)",
// "L3 2.5 MiB (too close to the L2: not measured)").
struct CacheEntry {
    int level = 0;
    QString name;  // "L1d", "L2"
    QString size;  // "48 KiB"
    bool notMeasured = false;
};

std::vector<CacheEntry> cacheEntries(const QString &levels)
{
    std::vector<CacheEntry> out;
    QString body = levels;
    body.remove(QStringLiteral(" (detected from the curve)"));
    for (const QString &part : body.split(QStringLiteral(" · "), Qt::SkipEmptyParts)) {
        const QStringList w = part.split(QLatin1Char(' '));
        if (w.size() < 3 || !w[0].startsWith(QLatin1Char('L')))
            continue;
        CacheEntry e;
        e.name = w[0];
        e.level = w[0].mid(1, 1).toInt();
        e.size = w[1] + QLatin1Char(' ') + w[2];
        e.notMeasured = part.contains(QLatin1StringView("not measured"));
        if (e.level > 0)
            out.push_back(e);
    }
    return out;
}

// "2 MiB pages", "4 KiB pages", "41% 2 MiB pages" (the subtitle and chart title).
QString pagesShort(const QString &pages)
{
    if (pages.startsWith(QLatin1StringView("mixed: ")))
        return pages.mid(7);
    QString p = pages.section(QStringLiteral(" ("), 0, 0);
    p.remove(QStringLiteral("transparent huge "));
    p.remove(QStringLiteral("large "));
    return p;
}

// Ticks every `step` GB/s from 0 past the largest load: "0 GB/s · 20 GB/s · …".
ChartAxis loadAxis(const Series &s)
{
    double max = 0;
    for (const QPointF &p : s.points)
        max = std::max(max, p.x());
    double step = 10;
    for (const double candidate : {10.0, 20.0, 25.0, 50.0, 100.0, 200.0, 250.0, 500.0}) {
        step = candidate;
        if (std::ceil(max / candidate) <= 4)
            break;
    }
    ChartAxis a;
    for (double x = 0; x <= std::max(step, std::ceil(max / step) * step) + 1e-9; x += step) {
        a.tickLabels << QStringLiteral("%1 GB/s").arg(x, 0, 'f', 0);
        a.tickValues << x;
    }
    return a;
}

// The latency chart's y ticks (D13, the mockup): the 1-2-5 sequence from
// the step at or below the fastest point to the first one at least twice
// the slowest, so the DRAM plateau doesn't touch the top.
ChartAxis latencyYAxis(const Series &s)
{
    ChartAxis a;
    a.scale = ChartAxis::Scale::Log10;
    double lo = INFINITY, hi = 0;
    for (const QPointF &p : s.points) {
        if (p.y() > 0) {
            lo = std::min(lo, p.y());
            hi = std::max(hi, p.y());
        }
    }
    if (!std::isfinite(lo) || hi <= 0)
        return a;
    QVector<double> steps;
    for (double decade = std::pow(10.0, std::floor(std::log10(lo))); decade <= hi * 20; decade *= 10) {
        for (const double m : {1.0, 2.0, 5.0})
            steps.append(m * decade);
    }
    for (const double v : std::as_const(steps)) {
        if (v * 2 < lo && v != steps.front())
            continue;
        if (!a.tickValues.isEmpty() && a.tickValues.last() >= 2 * hi)
            break;
        a.tickValues << v;
        a.tickLabels << QString::number(v, 'g', 6);
    }
    // Keep the lowest only if it is at or below the fastest point.
    while (a.tickValues.size() > 1 && a.tickValues[1] <= lo) {
        a.tickValues.removeFirst();
        a.tickLabels.removeFirst();
    }
    return a;
}

} // namespace

DetailLayout RamTest::detail(const TestResult &r) const
{
    DetailLayout d;
    d.headlineMetrics = {QStringLiteral("latency.dram"), QStringLiteral("bandwidth.read")};

    const QString latencyCore = coreOf(text(r, QStringLiteral("latency.core")));
    const QString stCore = coreOf(text(r, QStringLiteral("bandwidth.st.core")));
    const QString loadedCore = coreOf(text(r, QStringLiteral("loaded.core")));
    const QString pages = pagesShort(text(r, QStringLiteral("pages")));
    const QString type = text(r, QStringLiteral("config.type"));
    const auto speed = number(r, QStringLiteral("speed"));
    const QString memoryName = type.isEmpty() ? QString() : speed ? QStringLiteral("%1-%2").arg(type).arg(std::lround(*speed)) : type;
    const bool scored = r.status == Status::Ok || r.status == Status::Partial;

    // Subtitle: "Score 1,186 · DDR5-6000 · 2 MiB pages · latency on CPU 4".
    QStringList parts;
    if (r.score)
        parts << QStringLiteral("Score %1").arg(Units::format(*r.score, QStringLiteral("points"), false));
    else if (r.status == Status::Partial)
        parts << QStringLiteral("No RAM score");
    else if (r.status == Status::Cancelled)
        parts << QStringLiteral("Stopped");
    if (!memoryName.isEmpty())
        parts << memoryName;
    if (!pages.isEmpty())
        parts << pages;
    if (!latencyCore.isEmpty())
        parts << QStringLiteral("latency on %1").arg(latencyCore);
    d.subtitle = parts.join(QStringLiteral(" · "));

    // 1. KPIs: five boxes on one row at 1100 px (the mockup; the RAM score
    // only when there is one).
    KpisBlock kpis;
    if (r.score)
        kpis.items.append({QStringLiteral("RAM score"), DetailCell::literal(Units::format(*r.score, QStringLiteral("points"), false)),
                           QStringLiteral("points")});
    if (r.metric(QStringLiteral("latency.dram"))) {
        QStringList under;
        if (const QString at = text(r, QStringLiteral("latency.dram.at")); !at.isEmpty())
            under << at + QStringLiteral(" buffer");
        if (!latencyCore.isEmpty())
            under << latencyCore;
        kpis.items.append({QStringLiteral("DRAM latency"), DetailCell::literal(nsOf(r, QStringLiteral("latency.dram"), true)),
                           under.join(QStringLiteral(" · "))});
    }
    if (const auto read = number(r, QStringLiteral("bandwidth.read"))) {
        const auto threads = number(r, QStringLiteral("threads"));
        kpis.items.append({QStringLiteral("Read bandwidth"), DetailCell::literal(gbs(*read) + QStringLiteral(" GB/s")),
                           threads ? QStringLiteral("all %1 threads").arg(std::lround(*threads)) : QStringLiteral("all threads")});
    }
    if (r.metric(QStringLiteral("loaded.latency"))) {
        const auto load = number(r, QStringLiteral("loaded.load"));
        kpis.items.append({QStringLiteral("Loaded latency"), DetailCell::literal(nsOf(r, QStringLiteral("loaded.latency"), true)),
                           load ? QStringLiteral("at %1 GB/s load").arg(gbs(*load)) : QString()});
    }
    if (!memoryName.isEmpty()) {
        QStringList under;
        for (const char *k : {"config.total", "config.modules"}) {
            if (const QString t = text(r, QLatin1StringView(k)); !t.isEmpty())
                under << t;
        }
        kpis.items.append({QStringLiteral("Memory"), DetailCell::literal(memoryName), under.join(QStringLiteral(" · "))});
    }
    if (!kpis.items.isEmpty())
        d.blocks.append({{}, kpis});

    // 2. Sub-tests, full width. A Stopped or Interrupted run is never scored:
    // no Sub-score column at all.
    const bool hasL3 = r.metric(QStringLiteral("latency.l3")) != nullptr;
    TableBlock groups;
    groups.fullWidth = true;
    groups.columns = {QStringLiteral("Group"), QStringLiteral("Weight"), QStringLiteral("Result")};
    if (scored)
        groups.columns << QStringLiteral("Sub-score");
    const auto row = [&](const char *id, const QString &name, const char *weight, const QString &result) {
        const SubResult *s = sub(r, id);
        const bool ran = s && s->status == Status::Ok;
        QVector<DetailCell> cells{DetailCell::literal(name), DetailCell::literal(QLatin1StringView(weight)),
                                  DetailCell::literal(ran ? result : statusText(s))};
        if (scored)
            cells << DetailCell::literal(subScore(s));
        groups.rows.append(cells);
    };
    QString latency = QStringLiteral("DRAM %1").arg(nsOf(r, QStringLiteral("latency.dram"), true));
    if (hasL3)
        latency += QStringLiteral(" · L3 %1").arg(nsOf(r, QStringLiteral("latency.l3")));
    latency += QStringLiteral(" · L2 %1 · L1 %2 ns").arg(nsOf(r, QStringLiteral("latency.l2")), nsOf(r, QStringLiteral("latency.l1")));
    row("latency", QStringLiteral("Latency"), "50%", latency);
    row("bandwidth", QStringLiteral("Bandwidth"), "35%",
        QStringLiteral("read %1 · write %2 · copy %3 GB/s (all threads)\nread %4 · write %5 · copy %6 GB/s (single thread)")
            .arg(gbsOf(r, QStringLiteral("bandwidth.read")), gbsOf(r, QStringLiteral("bandwidth.write")),
                 gbsOf(r, QStringLiteral("bandwidth.copy")), gbsOf(r, QStringLiteral("bandwidth.st.read")),
                 gbsOf(r, QStringLiteral("bandwidth.st.write")), gbsOf(r, QStringLiteral("bandwidth.st.copy"))));
    row("loaded", QStringLiteral("Loaded latency"), "15%",
        QStringLiteral("%1 at %2 GB/s").arg(nsOf(r, QStringLiteral("loaded.latency"), true), gbsOf(r, QStringLiteral("loaded.load"))));
    if (scored) {
        groups.boldRows.append(int(groups.rows.size()));
        groups.rows.append({DetailCell::literal(QStringLiteral("RAM score")), DetailCell::literal(QStringLiteral("100%")),
                            DetailCell::literal(r.score ? QStringLiteral("weighted geometric mean")
                                                        : QStringLiteral("no score: not every part ran")),
                            DetailCell::literal(r.score ? Units::format(*r.score, QStringLiteral("points"), false) : kDash)});
    }
    d.blocks.append({QStringLiteral("Sub-tests"), groups});

    // 3. The latency chart (log-log) beside the Latency table (the
    // renderer's 3:2 split row), then its note.
    const std::vector<CacheEntry> caches = cacheEntries(text(r, QStringLiteral("cache.levels")));
    const bool fromCurve = text(r, QStringLiteral("cache.levels")).endsWith(QLatin1StringView("(detected from the curve)"));
    const QString where = latencyCore.isEmpty() ? QString() : latencyCore;
    if (const Series *curve = series(r, "latency_curve"); curve && !curve->points.isEmpty()) {
        LineChartBlock chart;
        chart.seriesKeys = {curve->key};
        chart.xAxis.scale = ChartAxis::Scale::Log2;
        chart.xAxis.tickLabels = {QStringLiteral("4K"),   QStringLiteral("16K"),  QStringLiteral("64K"), QStringLiteral("256K"),
                                  QStringLiteral("1M"),   QStringLiteral("4M"),   QStringLiteral("16M"), QStringLiteral("64M"),
                                  QStringLiteral("256M"), QStringLiteral("1G")};
        chart.xAxis.tickValues = {4, 16, 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576};  // KiB
        chart.yAxis = latencyYAxis(*curve);
        chart.yUnit = QStringLiteral("ns");
        // The OS cache sizes, labelled ("L1 48 KiB"); the targets are the dots.
        for (const CacheEntry &c : caches) {
            if (const auto kib = kibOf(c.size); kib && c.level <= 3)
                chart.annotations.append({*kib, QStringLiteral("L%1 %2").arg(c.level).arg(c.size)});
        }
        // Each scored target is a point of the series (its exact x is the
        // nearest point to the size its ".at" text names).
        for (const char *key : {"latency.l1.at", "latency.l2.at", "latency.l3.at", "latency.dram.at"}) {
            const auto kib = kibOf(text(r, QLatin1StringView(key)));
            if (!kib)
                continue;
            double best = 0, dist = INFINITY;
            for (const QPointF &p : curve->points) {
                const double dd = std::abs(std::log2(p.x()) - std::log2(*kib));
                if (dd < dist) {
                    dist = dd;
                    best = p.x();
                }
            }
            if (dist < 0.1)
                chart.markers.append(best);
        }
        QStringList title;
        if (!where.isEmpty())
            title << where;
        if (!pages.isEmpty())
            title << pages;
        d.blocks.append({title.isEmpty() ? QStringLiteral("Latency by buffer size")
                                         : QStringLiteral("Latency by buffer size (%1)").arg(title.join(QStringLiteral(" · "))),
                         chart});
    }
    if (r.metric(QStringLiteral("latency.l1")) || r.metric(QStringLiteral("latency.dram")) || !caches.empty()) {
        TableBlock lat;
        lat.columns = {QStringLiteral("Level"), QStringLiteral("Cache size"), QStringLiteral("Measured at"), QStringLiteral("Latency")};
        for (int k = 1; k <= 3; ++k) {
            const auto c = std::find_if(caches.begin(), caches.end(), [&](const CacheEntry &e) { return e.level == k; });
            const QString key = QStringLiteral("latency.l%1").arg(k);
            const QString at = text(r, key + QStringLiteral(".at"));
            QString value = r.metric(key) ? nsOf(r, key, true) : kDash;
            if (c == caches.end() && k == 3 && !caches.empty())
                value = fromCurve ? QStringLiteral("no L3 found on the curve") : QStringLiteral("no L3 on this core");
            else if (c != caches.end() && c->notMeasured)
                value = QStringLiteral("too close to the L%1").arg(k - 1);
            lat.rows.append({DetailCell::literal(QStringLiteral("L%1").arg(k)), DetailCell::literal(c != caches.end() ? c->size : kDash),
                             DetailCell::literal(at.isEmpty() ? kDash : at), DetailCell::literal(value)});
        }
        const QString dramAt = text(r, QStringLiteral("latency.dram.at"));
        lat.rows.append({DetailCell::literal(QStringLiteral("DRAM")), DetailCell::literal(kDash),
                         DetailCell::literal(dramAt.isEmpty() ? kDash : dramAt), DetailCell::literal(nsOf(r, QStringLiteral("latency.dram"), true))});
        d.blocks.append({where.isEmpty() ? QStringLiteral("Latency") : QStringLiteral("Latency (%1)").arg(where), lat});
        if (!caches.empty()) {
            const QString levels = text(r, QStringLiteral("cache.levels")).remove(QStringLiteral(" (detected from the curve)"));
            const QString source = fromCurve ? QStringLiteral("Cache sizes as found on the latency curve (the OS reports none): %1.").arg(levels)
                                             : QStringLiteral("Cache sizes as the OS reports them for %1: %2.").arg(where, levels);
            d.blocks.append({{}, NoteBlock{source + QStringLiteral(" Each level is measured in the middle of its plateau (the open "
                                                                   "dots); the chart's y axis is logarithmic."),
                                           Tone::Neutral}});
        }
    }

    // 4. Bandwidth, full width, then its note (and P9's cap-hit note).
    if (r.metric(QStringLiteral("bandwidth.read")) || r.metric(QStringLiteral("bandwidth.st.read"))) {
        const bool plain = r.metric(QStringLiteral("bandwidth.write.plain")) || r.metric(QStringLiteral("bandwidth.st.write.plain"));
        TableBlock bw;
        bw.fullWidth = true;
        bw.columns = {QStringLiteral("Threads"), QStringLiteral("Read"), QStringLiteral("Write"), QStringLiteral("Copy")};
        if (plain)
            bw.columns << QStringLiteral("Write, plain stores (not scored)") << QStringLiteral("Copy, plain stores (not scored)");
        const auto bwRow = [&](const QString &label, const QString &prefix) {
            QVector<DetailCell> cells{DetailCell::literal(label)};
            for (const char *op : {"read", "write", "copy"})
                cells << DetailCell::literal(gbsOf(r, prefix + QLatin1StringView(op)));
            if (plain) {
                for (const char *op : {"write.plain", "copy.plain"})
                    cells << DetailCell::literal(gbsOf(r, prefix + QLatin1StringView(op)));
            }
            bw.rows.append(cells);
        };
        const auto threads = number(r, QStringLiteral("threads"));
        bwRow(threads ? QStringLiteral("All threads (%1)").arg(std::lround(*threads)) : QStringLiteral("All threads"),
              QStringLiteral("bandwidth."));
        bwRow(stCore.isEmpty() ? QStringLiteral("Single thread") : QStringLiteral("Single thread (%1)").arg(stCore),
              QStringLiteral("bandwidth.st."));
        d.blocks.append({QStringLiteral("Bandwidth (GB/s)"), bw});
        const QString path = text(r, QStringLiteral("bandwidth.path")).section(QLatin1Char(' '), 0, 0);
        if (path == QLatin1StringView("NEON"))
            d.blocks.append({{}, NoteBlock{QStringLiteral("NEON loads and plain stores (ARM has no guaranteed non-temporal store). "
                                                          "Copy counts bytes read + written."),
                                           Tone::Neutral}});
        else if (!path.isEmpty())
            d.blocks.append({{}, NoteBlock{QStringLiteral("%1 loads. Write and copy are scored with non-temporal stores; the "
                                                          "plain-store columns are shown for reference. Copy counts bytes read + "
                                                          "written.")
                                               .arg(path),
                                           Tone::Neutral}});
    }
    if (const QString limited = text(r, QStringLiteral("bandwidth.limited")); !limited.isEmpty())
        d.blocks.append({{}, NoteBlock{limited, Tone::Warn}});

    // 5. Loaded latency: the chart beside its points, then its note. The
    // series is in level order (§5.4): the idle point (when the DRAM
    // latency was measured), then 6,000 ns ... full load.
    if (const Series *loaded = series(r, "loaded_curve"); loaded && !loaded->points.isEmpty()) {
        const int levels = int(std::size(ram::kLoadDelaysNs));
        const bool idle = loaded->points.size() == levels + 1;
        LineChartBlock chart;
        chart.seriesKeys = {loaded->key};
        chart.xAxis = loadAxis(*loaded);
        chart.yUnit = QStringLiteral("ns");
        if (idle)
            chart.annotations.append({loaded->points.first().x(), QStringLiteral("Idle %1 ns").arg(ns(loaded->points.first().y()))});
        chart.annotations.append({loaded->points.last().x(), QStringLiteral("Full load %1 ns").arg(ns(loaded->points.last().y()))});
        d.blocks.append({QStringLiteral("Loaded latency (latency against load bandwidth)"), chart});
        TableBlock points;
        points.columns = {QStringLiteral("Load level"), QStringLiteral("GB/s"), QStringLiteral("ns")};
        for (qsizetype i = 0; i < loaded->points.size(); ++i) {
            const qsizetype level = idle ? i - 1 : i;  // 0 = 6,000 ns ... levels - 1 = full load
            QString name;
            if (level < 0)
                name = QStringLiteral("Idle (no load)");
            else if (level >= levels - 1)
                name = QStringLiteral("Full load (scored)");
            else
                name = QStringLiteral("%1 ns delay").arg(Units::formatNumber(ram::kLoadDelaysNs[levels - 1 - level]));
            if (level >= levels - 1)
                points.boldRows.append(int(points.rows.size()));
            points.rows.append({DetailCell::literal(name), DetailCell::literal(gbs(loaded->points[i].x())),
                                DetailCell::literal(ns(loaded->points[i].y()))});
        }
        d.blocks.append({QStringLiteral("Loaded latency points"), points});
        if (const auto threads = number(r, QStringLiteral("loaded.threads"))) {
            const auto all = number(r, QStringLiteral("threads"));
            const long siblings = all ? std::lround(*all) - std::lround(*threads) - 1 : 0;
            const QString core = loadedCore.isEmpty() ? QStringLiteral("one core") : loadedCore;
            QString except;
            if (siblings == 1)
                except = QStringLiteral(" (not %1's SMT sibling)").arg(core);
            else if (siblings > 1)
                except = QStringLiteral(" (not %1's SMT siblings)").arg(core);
            d.blocks.append({{}, NoteBlock{QStringLiteral("The DRAM latency chase runs on %1 while %2 load threads on the other CPUs%3 "
                                                          "stream reads, pausing after every 4 KiB for the level's delay. Only full "
                                                          "load is scored.")
                                               .arg(core)
                                               .arg(std::lround(*threads))
                                               .arg(except),
                                           Tone::Neutral}});
        }
    }

    // 6. The memory configuration: the SMBIOS readout beside the derived
    // peak (two tables that pair), the Modules table, the no-SPD note.
    if (!type.isEmpty() || r.metric(QStringLiteral("config.total"))) {
        TableBlock config;
        config.columns = {QStringLiteral("Field"), QStringLiteral("Value")};
        const auto field = [&](const QString &label, const QString &value) {
            config.rows.append({DetailCell::literal(label), DetailCell::literal(value.isEmpty() ? kDash : value)});
        };
        field(QStringLiteral("Type"), type);
        field(QStringLiteral("Configured speed"),
              speed ? QStringLiteral("%1 MT/s").arg(QLocale(QLocale::English).toString(qlonglong(std::llround(*speed)))) : QString());
        field(QStringLiteral("Rated speed"), text(r, QStringLiteral("config.rated")));
        field(QStringLiteral("Total"), text(r, QStringLiteral("config.total")));
        field(QStringLiteral("Modules"), text(r, QStringLiteral("config.modules")));
        field(QStringLiteral("Ranks"), text(r, QStringLiteral("config.ranks")));
        field(QStringLiteral("Bus width"), text(r, QStringLiteral("config.width")));
        const auto channels = number(r, QStringLiteral("channels"));
        field(QStringLiteral("Channels (estimate)"), channels ? QString::number(std::lround(*channels)) : QString());
        d.blocks.append({QStringLiteral("Memory configuration (SMBIOS)"), config});
        if (const auto peak = number(r, QStringLiteral("config.peak"))) {
            TableBlock derived;
            derived.columns = {QStringLiteral("Field"), QStringLiteral("Value")};
            derived.rows.append({DetailCell::literal(QStringLiteral("Theoretical peak")),
                                 DetailCell::literal(QStringLiteral("%1 GB/s").arg(*peak, 0, 'f', 1))});
            derived.rows.append({DetailCell::literal(QStringLiteral("Derived as")),
                                 DetailCell::literal(QStringLiteral("%1 × %2 ÷ 8")
                                                         .arg(speed ? QStringLiteral("%1 MT/s")
                                                                          .arg(QLocale(QLocale::English).toString(qlonglong(std::llround(*speed))))
                                                                    : kDash,
                                                              text(r, QStringLiteral("config.width"))))});
            derived.rows.append({DetailCell::literal(QStringLiteral("Measured read (all threads)")),
                                 DetailCell::literal(r.metric(QStringLiteral("bandwidth.read"))
                                                         ? gbsOf(r, QStringLiteral("bandwidth.read")) + QStringLiteral(" GB/s")
                                                         : kDash)});
            const auto efficiency = number(r, QStringLiteral("config.efficiency"));
            derived.rows.append({DetailCell::literal(QStringLiteral("Read efficiency")),
                                 DetailCell::literal(efficiency ? QStringLiteral("%1%").arg(std::lround(*efficiency)) : kDash)});
            d.blocks.append({QStringLiteral("Theoretical peak (derived from SMBIOS)"), derived});
        }
        TableBlock modules;
        modules.fullWidth = true;
        modules.columns = {QStringLiteral("Slot"),  QStringLiteral("Size"),  QStringLiteral("Type"),       QStringLiteral("Speed"),
                           QStringLiteral("Ranks"), QStringLiteral("Maker"), QStringLiteral("Part number")};
        for (int i = 1; i <= 16; ++i) {
            const QString line = text(r, QStringLiteral("config.module.%1").arg(i));
            if (line.isEmpty())
                break;
            QVector<DetailCell> cells;
            for (const QString &f : ram::moduleFields(line))
                cells << DetailCell::literal(f);
            modules.rows.append(cells);
        }
        if (!modules.rows.isEmpty())
            d.blocks.append({QStringLiteral("Modules"), modules});
        d.blocks.append({{}, NoteBlock{QStringLiteral("As reported by the firmware (SMBIOS). SynBench doesn't read SPD, so timings "
                                                      "aren't shown."),
                                       Tone::Neutral}});
    } else if (const QString source = text(r, QStringLiteral("config.source")); source.startsWith(QLatin1StringView("not available: "))) {
        d.blocks.append({{}, NoteBlock{QStringLiteral("Memory details not available: %1. The test still ran%2.")
                                           .arg(source.mid(15), r.score ? QStringLiteral(" and is scored") : QString()),
                                       Tone::Neutral}});
    }

    // 7. Notes: the run's status, the page mode, the conditions.
    if (r.status == Status::Partial) {
        QStringList problems;
        for (const SubResult &s : r.subResults) {
            if (s.status == Status::Ok)
                continue;
            QString name = s.id;
            for (const SubTestInfo &i : m_info.subTests) {
                if (i.id == s.id)
                    name = i.name;
            }
            problems << QStringLiteral("%1: %2").arg(name, s.reason.isEmpty() ? statusText(&s) : s.reason);
        }
        QString note = QStringLiteral("No RAM score: not every part ran.");
        if (!problems.isEmpty())
            note += QLatin1Char(' ') + problems.join(QStringLiteral("; ")) + QLatin1Char('.');
        d.blocks.append({{}, NoteBlock{note, Tone::Warn}});
    } else if (r.status == Status::Cancelled) {
        d.blocks.append({{}, NoteBlock{QStringLiteral("Stopped before the end: the parts that finished are shown, without scores."),
                                       Tone::Warn}});
    } else if (r.status == Status::TimedOut || r.status == Status::Interrupted) {
        d.blocks.append({{}, NoteBlock{QStringLiteral("The test didn't finish: the parts that finished are shown, without scores."),
                                       Tone::Warn}});
    }
    const QString pageText = text(r, QStringLiteral("pages"));
    if (pageText.startsWith(QLatin1StringView("mixed: "))) {
        const auto percent = number(r, QStringLiteral("pages.huge"));
        const QString huge = pageText.section(QLatin1Char('%'), 1).trimmed().section(QStringLiteral(" pages"), 0, 0);
        d.blocks.append({{}, NoteBlock{QStringLiteral("Only %1% of the latency buffer got %2 pages, so DRAM and loaded latency may "
                                                      "read higher than on a full huge-page run. The score is not adjusted.")
                                           .arg(percent ? std::lround(*percent) : 0)
                                           .arg(huge),
                                       Tone::Warn}});
    } else if (pageText.startsWith(QLatin1StringView("4 KiB")) || pageText.startsWith(QLatin1StringView("16 KiB"))
               || pageText.startsWith(QLatin1StringView("64 KiB"))) {
        d.blocks.append({{}, NoteBlock{QStringLiteral("This run used %1 pages: DRAM and loaded latency include TLB misses, so they "
                                                      "read higher than on a huge-page run. The score is not adjusted.")
                                           .arg(pageText.section(QStringLiteral(" pages"), 0, 0)),
                                       Tone::Warn}});
    }
    if (r.metric(QStringLiteral("emulated")))
        d.blocks.append({{}, NoteBlock{QStringLiteral("This x64 build ran under emulation (Prism): latency is only roughly right. "
                                                      "Run the ARM64 build for real numbers."),
                                       Tone::Warn}});
    if (text(r, QStringLiteral("power.source")) == QLatin1StringView("Battery"))
        d.blocks.append({{}, NoteBlock{QStringLiteral("On battery"), Tone::Warn}});
    // No background-load note: Dave, 2026-10-20 (the metric stays, for the
    // repeatability script).
    return d;
}

} // namespace sb::tests
