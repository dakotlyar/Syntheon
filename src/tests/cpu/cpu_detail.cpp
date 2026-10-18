// SPDX-License-Identifier: GPL-3.0-or-later
// The CPU test's Detail page (Phase 2 design §10, task 16): the layout of
// the approved mockup ([[Phase 2 - CPU Mockup]], 2026-10-16, with Dave's
// single-thread core wording of 2026-10-18), built only from the existing
// blocks, for a completed, a Partial and a Stopped result. Pure over the
// stored result.
#include "core/units.h"
#include "tests/cpu/compress/codec.h"
#include "tests/cpu/cpu_test.h"
#include "tests/cpu/cpu_windows.h"

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace sb::tests {

namespace {

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

// "148,600 MOps/s" (or a dash).
QString withUnit(const TestResult &r, const QString &key)
{
    const Metric *m = r.metric(key);
    if (!m || !m->isNumeric())
        return QStringLiteral("—");
    return Units::format(std::get<double>(m->value), m->unit);
}

// "6,380" (the unit is in the table's title).
QString bare(const TestResult &r, const QString &key)
{
    const auto v = number(r, key);
    return v ? Units::formatNumber(*v, QStringLiteral("MB/s")) : QStringLiteral("—");
}

const SubResult *sub(const TestResult &r, const char *id)
{
    for (const SubResult &s : r.subResults) {
        if (s.id == QLatin1StringView(id))
            return &s;
    }
    return nullptr;
}

// A group's Result cell when it has no numbers.
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
    return QStringLiteral("—");
}

QString subScore(const SubResult *s)
{
    return s && s->score ? Units::format(*s->score, QStringLiteral("points"), false) : QStringLiteral("—");
}

// "AVX-512 (512-bit)" -> {"AVX-512", "512-bit"}.
std::pair<QString, QString> splitParenthesis(const QString &s)
{
    const qsizetype open = s.indexOf(QStringLiteral(" ("));
    if (open < 0 || !s.endsWith(QLatin1Char(')')))
        return {s, {}};
    return {s.left(open), s.mid(open + 2, s.size() - open - 3)};
}

// "AES-NI + VAES, SHA-NI" -> {"AES-NI", "+ VAES · SHA-NI"}.
std::pair<QString, QString> splitCryptoPath(const QString &s)
{
    const QStringList parts = s.split(QStringLiteral(", "));
    const QStringList first = parts.value(0).split(QStringLiteral(" + "));
    QStringList rest;
    for (qsizetype i = 1; i < first.size(); ++i)
        rest << QStringLiteral("+ ") + first[i];
    for (qsizetype i = 1; i < parts.size(); ++i)
        rest << parts[i];
    return {first.value(0), rest.join(QStringLiteral(" · "))};
}

// "Peak temperature (k10temp Tctl)" -> "k10temp Tctl".
QString sensorOf(const QString &label)
{
    return splitParenthesis(label).second;
}

// The cores in single.cores ("integer 0,5,5,0,5 · FP …"), in first-use order.
QStringList coresUsed(const QString &cores)
{
    QStringList out;
    for (const QString &part : cores.split(QStringLiteral(" · "), Qt::SkipEmptyParts)) {
        for (const QString &c : part.section(QLatin1Char(' '), -1).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            if (c != QLatin1StringView("-") && !out.contains(c))
                out << c;
        }
    }
    return out;
}

const Series *series(const TestResult &r, const char *key)
{
    for (const Series &s : r.series) {
        if (s.key == QLatin1StringView(key))
            return &s;
    }
    return nullptr;
}

// Ticks every 60 s and at the end ("0 s · 60 s · … · 270 s").
ChartAxis timeAxis(const Series &s)
{
    ChartAxis a;
    if (s.points.isEmpty())
        return a;
    const double end = s.points.last().x();
    for (int t = 0; t < end - 15; t += 60) {
        a.tickLabels << QStringLiteral("%1 s").arg(t);
        a.tickValues << t;
    }
    a.tickLabels << QStringLiteral("%1 s").arg(std::lround(end));
    a.tickValues << end;
    return a;
}

} // namespace

DetailLayout CpuTest::detail(const TestResult &r) const
{
    DetailLayout d;
    d.headlineMetrics = {QStringLiteral("single.mark")};

    // Subtitle: "Score 875 · AVX-512 path · 24 threads · Throttling detected".
    QStringList parts;
    if (r.score)
        parts << QStringLiteral("Score %1").arg(Units::format(*r.score, QStringLiteral("points"), false));
    else if (r.status == Status::Partial)
        parts << QStringLiteral("No CPU score");
    else if (r.status == Status::Cancelled)
        parts << QStringLiteral("Stopped");
    const QString simdPath = text(r, QStringLiteral("simd.path"));
    if (!simdPath.isEmpty())
        parts << splitParenthesis(simdPath).first + QStringLiteral(" path");
    if (const auto threads = number(r, QStringLiteral("threads")))
        parts << QStringLiteral("%1 threads").arg(std::lround(*threads));
    const QString throttle = text(r, QStringLiteral("throttle"));
    if (!throttle.isEmpty())
        parts << QStringLiteral("Throttling %1").arg(throttle.toLower());
    d.subtitle = parts.join(QStringLiteral(" · "));

    // 1. KPIs.
    KpisBlock kpis;
    if (r.metric(QStringLiteral("single.mark")))
        kpis.items.append({QStringLiteral("Single-thread mark"), DetailCell::metric(QStringLiteral("single.mark")), QStringLiteral("points")});
    if (!simdPath.isEmpty()) {
        const auto [path, width] = splitParenthesis(simdPath);
        kpis.items.append({QStringLiteral("SIMD path"), DetailCell::literal(path), width});
    }
    if (const QString crypto = text(r, QStringLiteral("crypto.path")); !crypto.isEmpty()) {
        const auto [main, rest] = splitCryptoPath(crypto);
        kpis.items.append({QStringLiteral("Encryption path"), DetailCell::literal(main), rest});
    }
    if (r.metric(QStringLiteral("threads")))
        kpis.items.append({QStringLiteral("Threads used"), DetailCell::metric(QStringLiteral("threads")), QStringLiteral("logical CPUs")});
    if (const Metric *t = r.metric(QStringLiteral("temp.max")); t && t->isNumeric())
        kpis.items.append({QStringLiteral("Peak temperature"),
                           DetailCell::literal(Units::format(std::get<double>(t->value), QStringLiteral("°C"))),
                           sensorOf(t->label)});
    if (!kpis.items.isEmpty())
        d.blocks.append({{}, kpis});

    // 2. The groups.
    TableBlock groups;
    groups.columns = {QStringLiteral("Group"), QStringLiteral("Weight"), QStringLiteral("Result"), QStringLiteral("Sub-score")};
    const auto row = [&](const char *id, const QString &name, const char *weight, const QString &result) {
        const SubResult *s = sub(r, id);
        const bool ran = s && s->status == Status::Ok;
        groups.rows.append({DetailCell::literal(name), DetailCell::literal(QLatin1StringView(weight)),
                            DetailCell::literal(ran ? result : statusText(s)), DetailCell::literal(subScore(s))});
    };
    row("int", QStringLiteral("Integer"), "25%", withUnit(r, QStringLiteral("int.rate")));
    row("fp", QStringLiteral("Floating point + primes"), "25%",
        withUnit(r, QStringLiteral("fp.rate")) + QStringLiteral("\nprimes ") + withUnit(r, QStringLiteral("fp.primes")));
    row("simd", simdPath.isEmpty() ? QStringLiteral("SIMD") : QStringLiteral("SIMD (%1)").arg(splitParenthesis(simdPath).first),
        "15%", withUnit(r, QStringLiteral("simd.rate")));
    row("compress", QStringLiteral("Compression"), "10%", QStringLiteral("4 codecs, see below"));
    row("crypto", QStringLiteral("Encryption (hardware path)"), "10%",
        QStringLiteral("AES-256-GCM ") + withUnit(r, QStringLiteral("crypto.aes_gcm")) + QStringLiteral("\nSHA-256 ")
            + withUnit(r, QStringLiteral("crypto.sha256")));
    row("sort", QStringLiteral("Sorting"), "10%",
        QStringLiteral("64-bit ") + withUnit(r, QStringLiteral("sort.u64")) + QStringLiteral("\nstrings ")
            + withUnit(r, QStringLiteral("sort.str")));
    row("physics", QStringLiteral("Physics (n-body, FP64)"), "5%", withUnit(r, QStringLiteral("physics.nbody")));
    groups.rows.append({DetailCell::literal(QStringLiteral("CPU score")), DetailCell::literal(QStringLiteral("100%")),
                        DetailCell::literal(r.score ? QStringLiteral("weighted geometric mean")
                                                    : QStringLiteral("no score: not every sub-test ran")),
                        DetailCell::literal(r.score ? Units::format(*r.score, QStringLiteral("points"), false)
                                                    : QStringLiteral("—"))});
    d.blocks.append({QStringLiteral("Sub-tests (all threads)"), groups});

    // The throttling note sits between the two tables on purpose (the
    // renderer pairs consecutive tables; these two need the full width).
    if (const QString note = text(r, QStringLiteral("throttle.note")); !note.isEmpty())
        d.blocks.append({{}, NoteBlock{note, Tone::Warn}});

    // 4. Encryption, then its note.
    if (r.metric(QStringLiteral("crypto.aes_gcm")) || r.metric(QStringLiteral("crypto.sha256"))) {
        TableBlock enc;
        enc.columns = {QStringLiteral("Algorithm"), QStringLiteral("Hardware path (scored)"),
                       QStringLiteral("Software path (not scored)"), QStringLiteral("Speed-up")};
        const auto encRow = [&](const QString &name, const QString &hw, const QString &sw) {
            const auto h = number(r, hw), s = number(r, sw);
            const QString speedUp = h && s && *s > 0 ? QStringLiteral("%1×").arg(*h / *s, 0, 'f', 1) : QStringLiteral("—");
            enc.rows.append({DetailCell::literal(name), DetailCell::literal(withUnit(r, hw)), DetailCell::literal(withUnit(r, sw)),
                             DetailCell::literal(speedUp)});
        };
        encRow(QStringLiteral("AES-256-GCM"), QStringLiteral("crypto.aes_gcm"), QStringLiteral("crypto.aes_gcm.sw"));
        encRow(QStringLiteral("SHA-256"), QStringLiteral("crypto.sha256"), QStringLiteral("crypto.sha256.sw"));
        d.blocks.append({QStringLiteral("Encryption"), enc});
        d.blocks.append({{}, NoteBlock{QStringLiteral("Only the hardware path is scored. The software path (AES and SHA "
                                                      "instructions off, SIMD on) is shown for reference."),
                                       Tone::Neutral}});
    }

    // 3 and 5: Compression and Single-thread, a side-by-side pair.
    TableBlock comp;
    comp.columns = {QStringLiteral("Codec"), QStringLiteral("Compress"), QStringLiteral("Decompress"), QStringLiteral("Ratio")};
    bool anyCodec = false;
    for (const cpu::Codec c : cpu::kCodecs) {
        const QString key = QString::fromLatin1(cpu::codecKey(c));
        const auto ratio = number(r, QStringLiteral("compress.%1.ratio").arg(key));
        anyCodec = anyCodec || r.metric(QStringLiteral("compress.%1.c").arg(key));
        comp.rows.append({DetailCell::literal(QString::fromLatin1(cpu::codecLabel(c))),
                          DetailCell::literal(bare(r, QStringLiteral("compress.%1.c").arg(key))),
                          DetailCell::literal(bare(r, QStringLiteral("compress.%1.d").arg(key))),
                          DetailCell::literal(ratio ? QString::number(*ratio, 'f', 2) : QStringLiteral("—"))});
    }
    if (anyCodec)
        d.blocks.append({QStringLiteral("Compression (MB/s)"), comp});

    if (r.metric(QStringLiteral("single.int")) || r.metric(QStringLiteral("single.fp"))
        || r.metric(QStringLiteral("single.compress"))) {
        const QStringList cores = coresUsed(text(r, QStringLiteral("single.cores")));
        QString title;
        if (text(r, QStringLiteral("single.cpu")).startsWith(QLatin1StringView("unpinned")) || cores.isEmpty())
            title = QStringLiteral("Single-thread · unpinned");
        else if (cores.size() == 1)
            title = QStringLiteral("Single-thread · pinned to CPU %1").arg(cores.front());
        else
            title = QStringLiteral("Single-thread · pinned per repetition (CPU %1)").arg(cores.join(QStringLiteral(", ")));
        TableBlock st;
        st.columns = {QStringLiteral("Part"), QStringLiteral("Result")};
        st.rows.append({DetailCell::literal(QStringLiteral("Integer")), DetailCell::literal(withUnit(r, QStringLiteral("single.int")))});
        st.rows.append({DetailCell::literal(QStringLiteral("Floating point")), DetailCell::literal(withUnit(r, QStringLiteral("single.fp")))});
        st.rows.append({DetailCell::literal(QStringLiteral("Compression (geo-mean of 8)")),
                        DetailCell::literal(withUnit(r, QStringLiteral("single.compress")))});
        const auto mark = number(r, QStringLiteral("single.mark"));
        st.rows.append({DetailCell::literal(QStringLiteral("Single-thread mark")),
                        DetailCell::literal(mark ? Units::format(*mark, QStringLiteral("points"), false) : QStringLiteral("—"))});
        d.blocks.append({title, st});
    }

    // 6. The two charts, one series each, with the windows labelled.
    std::map<int, double> starts;  // window code -> start (s)
    if (const Series *w = series(r, "telemetry.windows")) {
        for (const QPointF &p : w->points)
            starts.emplace(int(std::lround(p.y())), p.x());
    }
    // The throttled window, from throttle.detail ("xz compression: clock …").
    int throttled = 0;
    QString throttledLabel;
    if (const QString detail = text(r, QStringLiteral("throttle.detail")); !detail.isEmpty()) {
        for (const auto &[code, x] : starts) {
            const QString name = windowName(code);
            if (!name.isEmpty() && detail.startsWith(name + QStringLiteral(": "))) {
                throttled = code;
                const qsizetype open = detail.lastIndexOf(QStringLiteral("(")), close = detail.lastIndexOf(QStringLiteral(")"));
                const QString drop = open >= 0 && close > open ? detail.mid(open, close - open + 1) : QString();
                const bool codec = code >= kWinCompress && code < kWinCompress + 2 * int(cpu::kCodecCount);
                throttledLabel = (codec ? name.section(QLatin1Char(' '), 0, 0) : name) + (drop.isEmpty() ? QString() : QStringLiteral(" ") + drop);
            }
        }
    }
    const auto mark = [&](QVector<ChartAnnotation> &out, int code, const QString &label) {
        if (const auto it = starts.find(code); it != starts.end())
            out.append({it->second, label});
    };
    if (const Series *clock = series(r, "telemetry.clock"); clock && !clock->points.isEmpty()) {
        LineChartBlock chart;
        chart.seriesKeys = {clock->key};
        chart.xAxis = timeAxis(*clock);
        chart.yUnit = QStringLiteral("MHz");
        mark(chart.annotations, kWinSingleInt, QStringLiteral("Single-thread"));
        mark(chart.annotations, kWinInt, QStringLiteral("Integer"));
        const bool compressThrottled = throttled >= kWinCompress && throttled < kWinCompress + 2 * int(cpu::kCodecCount);
        if (!compressThrottled)
            mark(chart.annotations, kWinCompress, QStringLiteral("Compression"));
        mark(chart.annotations, kWinAesHw, QStringLiteral("Encryption"));
        mark(chart.annotations, kWinPhysics, QStringLiteral("Physics"));
        if (throttled)
            mark(chart.annotations, throttled, throttledLabel);
        std::sort(chart.annotations.begin(), chart.annotations.end(),
                  [](const ChartAnnotation &a, const ChartAnnotation &b) { return a.x < b.x; });
        d.blocks.append({QStringLiteral("Clock (CPUs running the test, MHz)"), chart});
    }
    if (const Series *temp = series(r, "telemetry.temp"); temp && !temp->points.isEmpty()) {
        LineChartBlock chart;
        chart.seriesKeys = {temp->key};
        chart.xAxis = timeAxis(*temp);
        chart.yUnit = QStringLiteral("°C");
        mark(chart.annotations, kWinSingleInt, QStringLiteral("Single-thread"));
        mark(chart.annotations, kWinAesHw, QStringLiteral("Encryption"));
        // The peak, labelled with its value.
        const auto peak = std::max_element(temp->points.cbegin(), temp->points.cend(),
                                           [](const QPointF &a, const QPointF &b) { return a.y() < b.y(); });
        chart.annotations.append({peak->x(), Units::format(peak->y(), QStringLiteral("°C"))});
        std::sort(chart.annotations.begin(), chart.annotations.end(),
                  [](const ChartAnnotation &a, const ChartAnnotation &b) { return a.x < b.x; });
        const QString sensor = splitParenthesis(temp->label).second;
        d.blocks.append({sensor.isEmpty() ? QStringLiteral("Temperature (°C)") : QStringLiteral("Temperature (%1, °C)").arg(sensor),
                         chart});
    }

    // 7. Notes.
    if (r.status == Status::Partial) {
        QStringList problems;
        for (const SubResult &s : r.subResults) {
            if (s.status != Status::Ok)
                problems << QStringLiteral("%1: %2").arg(s.id, s.reason.isEmpty() ? statusText(&s) : s.reason);
        }
        d.blocks.append({{}, NoteBlock{QStringLiteral("No CPU score: not every sub-test ran. ") + problems.join(QStringLiteral("; ")),
                                       Tone::Warn}});
    } else if (r.status == Status::Cancelled) {
        d.blocks.append({{}, NoteBlock{QStringLiteral("Stopped before the end: the parts that finished are shown, and "
                                                      "there is no CPU score."),
                                       Tone::Warn}});
    }
    if (r.metric(QStringLiteral("emulated")))
        d.blocks.append({{}, NoteBlock{QStringLiteral("This x64 build is running under emulation on an ARM64 PC; install the "
                                                      "ARM64 build for native results."),
                                       Tone::Warn}});
    if (text(r, QStringLiteral("power.source")) == QLatin1StringView("Battery"))
        d.blocks.append({{}, NoteBlock{QStringLiteral("On battery"), Tone::Warn}});
    if (const auto load = number(r, QStringLiteral("background.load")); load && *load >= 10)
        d.blocks.append({{}, NoteBlock{QStringLiteral("Background load %1% before the run").arg(std::lround(*load)), Tone::Warn}});
    return d;
}

} // namespace sb::tests
