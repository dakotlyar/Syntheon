// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU test's Detail page (Phase 4a design §15, task 34): the content of
// the approved mockup ([[Phase 4a - GPU Mockup]], which wins where it differs
// from §15), in the order §15 lists it. The renderer arranges the page (D24:
// the KPIs and the run's warnings, every chart, then the collapsed tables), so
// only the order of the blocks matters here: a neutral note is drawn with the
// block before it, and the Draw calls note follows the "Draw calls by API"
// table so that it lands under that scene's chart. Built only from the
// existing blocks; pure over the stored result.
//
// The numbers in the summaries are formatted exactly as the same numbers in
// the cells (`value()` below is the app's formatMetric without the unit; the
// tests library cannot link the app library).
#include "core/units.h"
#include "tests/gpu/gpu_test.h"
#include "tests/gpu/model/ids.h"

#include <QLocale>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <optional>

namespace sb::tests {

using namespace sb::gpu;

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

// formatMetric(r, key, false), or "" when the metric is missing (so a missing part can be left out).
QString value(const TestResult &r, const QString &key)
{
    const Metric *m = r.metric(key);
    if (!m || !m->isNumeric())
        return {};
    return Units::format(std::get<double>(m->value), m->unit, false);
}

QString valueOrDash(const TestResult &r, const QString &key)
{
    const QString v = value(r, key);
    return v.isEmpty() ? kDash : v;
}

// Two decimals for the GPU / CPU milliseconds columns (the mockup: 2.35, 10.40).
QString ms(const TestResult &r, const QString &key)
{
    const auto v = number(r, key);
    return v ? QString::number(*v, 'f', 2) : kDash;
}

// The tables and charts say "Particles"; the progress and the metric labels say "Particles + post" (the mockup).
QString sceneLabel(SceneId s)
{
    return s == SceneId::Particles ? QStringLiteral("Particles") : sceneName(s);
}

const SubResult *sub(const TestResult &r, const QString &id)
{
    for (const SubResult &s : r.subResults) {
        if (s.id == id)
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
    return s && s->score ? Units::format(*s->score, QStringLiteral("points"), false) : QString();
}

bool hasSeries(const TestResult &r, const QString &key)
{
    return std::any_of(r.series.begin(), r.series.end(), [&](const Series &s) { return s.key == key; });
}

QString apiStatus(const TestResult &r, const QString &key)
{
    return text(r, apiStatusKey(key, u"status"));
}

// "OK" / "N/A" / "Error" / "Stopped" from "OK", "N/A: <reason>", "Error: <reason>", "Stopped".
QString statusWord(const QString &status)
{
    if (status.startsWith(QLatin1StringView("N/A")))
        return QStringLiteral("N/A");
    if (status.startsWith(QLatin1StringView("Error")))
        return QStringLiteral("Error");
    return status;
}

QString statusReason(const QString &status)
{
    const qsizetype at = status.indexOf(QStringLiteral(": "));
    return at < 0 ? QString() : status.mid(at + 2);
}

bool errored(const TestResult &r, const QString &apiKeyText)
{
    return apiStatus(r, apiKeyText).startsWith(QLatin1StringView("Error"));
}

// The 3D APIs with a column / row in the tables: all but those that said N/A (Linux shows Vulkan and OpenGL only).
QVector<ApiId> shownApis(const TestResult &r)
{
    QVector<ApiId> out;
    for (const ApiId a : kApis) {
        const QString status = apiStatus(r, apiKey(a));
        bool data = false;
        for (const SceneId s : kScenes)
            data = data || r.metric(sceneApiKey(a, s, u"fps"));
        if (data || (!status.isEmpty() && !status.startsWith(QLatin1StringView("N/A"))))
            out.append(a);
    }
    return out;
}

QString shortOf(const QString &apiNameText)
{
    for (const ApiId a : kApis) {
        if (apiName(a) == apiNameText)
            return apiShort(a);
    }
    return apiNameText;
}

std::optional<ApiId> apiOfName(const QString &name)
{
    for (const ApiId a : kApis) {
        if (apiName(a) == name)
            return a;
    }
    return std::nullopt;
}

// The GPU name without its maker: "AMD Radeon RX 7800 XT" -> "RX 7800 XT".
QString shortGpuName(QString name)
{
    for (const QString &prefix : {QStringLiteral("AMD "), QStringLiteral("NVIDIA "), QStringLiteral("Intel(R) "), QStringLiteral("Intel ")}) {
        if (name.startsWith(prefix)) {
            name.remove(0, int(prefix.size()));
            break;
        }
    }
    name.remove(QStringLiteral("(TM)"));
    if (const qsizetype paren = name.indexOf(QStringLiteral(" (")); paren > 0)  // "(RADV STRIX_HALO)": the driver's tag
        name.truncate(paren);
    if (name.startsWith(QStringLiteral("GeForce ")))
        name.remove(0, 8);
    else if (name.startsWith(QStringLiteral("Radeon RX ")) || name.startsWith(QStringLiteral("Radeon PRO ")))
        name.remove(0, 7);
    if (name.endsWith(QStringLiteral(" Graphics")))
        name.chop(9);
    return name.simplified();
}

QString duration(qint64 durationMs)
{
    const qint64 s = (durationMs + 500) / 1000;
    return s >= 60 ? QStringLiteral("%1 min %2 s").arg(s / 60).arg(s % 60) : QStringLiteral("%1 s").arg(s);
}

QString plural(int n, const QString &what)
{
    return QStringLiteral("%1 %2%3").arg(n).arg(what, n == 1 ? QString() : QStringLiteral("s"));
}

// "No GPU score: <Api> failed during <Workload>: <reason>" (and its siblings; "in" in older runs) taken apart.
struct Failure {
    QString api, workload, why;
    bool matched = false;
    bool drew() const { return why.startsWith(QStringLiteral("drew ")); }
};

Failure failureOf(const QString &statusReason)
{
    QString s = statusReason;
    if (s.startsWith(QStringLiteral("No GPU score: ")))
        s = s.mid(14);
    static const QRegularExpression re(QStringLiteral("^(.+?) failed (?:in|during) (.+?): (.+)$"));
    const QRegularExpressionMatch m = re.match(s);
    if (!m.hasMatch())
        return {};
    return {m.captured(1), m.captured(2), m.captured(3), true};
}

// What follows "<name> failed during " in a sub-test's reason, or "<name> failed in " in a run stored before Zeb Z1 (so old
// runs stay readable); nullopt when the reason isn't about this name.
std::optional<QString> afterFailed(const QString &reason, const QString &name)
{
    for (const QString &word : {QStringLiteral(" failed during "), QStringLiteral(" failed in ")}) {
        const QString prefix = name + word;
        if (reason.startsWith(prefix))
            return reason.mid(int(prefix.size()));
    }
    return std::nullopt;
}

// The Note column of an API that errored: "failed during <workload>: <reason>", or "drew …" for a signature outlier.
// The status says where since checkpoint 6 ("Error: failed during Shading: …", every errored API); an older run's
// status has only the reason, and its workload is found for the API the sub-test's reason names.
QString errorNote(const TestResult &r, const QString &name, const QString &status)
{
    const QString reason = statusReason(status);
    if (reason.startsWith(QStringLiteral("drew ")) || reason.startsWith(QStringLiteral("failed during ")))
        return reason;
    for (const SubResult &s : r.subResults) {
        if (s.status != Status::Error)
            continue;
        if (const auto rest = afterFailed(s.reason, name))
            return QStringLiteral("failed during ") + *rest;
    }
    return reason.isEmpty() ? QStringLiteral("failed") : QStringLiteral("failed: ") + reason;
}

// The workload an errored API failed in ("Shading", "Textured quads", "start-up"), from its status since checkpoint 6
// ("Error: failed during Shading: …"), else from the sub-test reason that names it; empty when neither says.
QString failedWorkload(const TestResult &r, ApiId a)
{
    const QString reason = statusReason(apiStatus(r, apiKey(a)));
    static const QRegularExpression during(QStringLiteral("^failed during (.+?): "));
    if (const QRegularExpressionMatch m = during.match(reason); m.hasMatch())
        return m.captured(1);
    for (const SubResult &s : r.subResults) {
        if (s.status != Status::Error)
            continue;
        if (const auto rest = afterFailed(s.reason, apiName(a)))
            return rest->section(QStringLiteral(": "), 0, 0);
    }
    return {};
}

// A cell with no number for an API that errored: "Error" in the workload it failed in, "—" in the ones it never ran
// (Zeb nit: as a failed Direct2D's cells). An API that failed at start-up ran nothing, so every cell reads "Error" (AP-5 L2,
// as before), and so does an older run that doesn't say where.
QString missingCell(const TestResult &r, ApiId a, const QString &workload)
{
    if (!errored(r, apiKey(a)))
        return kDash;
    const QString where = failedWorkload(r, a);
    return where.isEmpty() || where == workload || where == QLatin1StringView("start-up") ? QStringLiteral("Error") : kDash;
}

// " (why)" after a sentence, or ": why" when the reason has parentheses of its own (Zeb nit: no
// "(too few frames to measure (19))").
QString becauseOf(const QString &why)
{
    return why.contains(QLatin1Char('(')) ? QStringLiteral(": ") + why : QStringLiteral(" (%1)").arg(why);
}

TableBlock table(const QStringList &columns)
{
    TableBlock t;
    t.columns = columns;
    return t;
}

} // namespace

DetailLayout GpuTest::detail(const TestResult &r) const
{
    // §12, AP-5 L8/L1: an old v0 stub run renders through its stored labels, as before task 34. It is told apart by what
    // it lacks and has: no real run's finish() leaves out `gpu.name`, and a stub's metrics are all there is to show. An
    // orchestrator exception or an Interrupted run has no metrics at all, so it keeps this page and says why in the Warn
    // note at the top.
    if (!r.metric(QStringLiteral("gpu.name")) && !r.metrics.isEmpty()
        && std::none_of(kApis.begin(), kApis.end(), [&](ApiId a) { return r.metric(apiStatusKey(apiKey(a), u"status")); }))
        return genericDetailLayout(r);
    DetailLayout d;
    // AP-5 M1: a result with no metrics that was never measured (not available before the probe, or not run) has no GPU
    // page to show; its state and reason are the page, as the generic layout showed them (withNotAvailableState turns the
    // N/A one into the NaStateBlock).
    if (r.metrics.isEmpty() && (r.status == Status::NotAvailable || r.status == Status::NotRun)) {
        const bool na = r.status == Status::NotAvailable;
        d.subtitle = na ? QStringLiteral("Not available on this machine") : QStringLiteral("Not run");
        const QString state = na ? QStringLiteral("N/A") : QStringLiteral("Not run");
        d.blocks.append({{}, NoteBlock{r.statusReason.isEmpty() ? state : QStringLiteral("%1: %2").arg(state, r.statusReason), Tone::Na}});
        return d;
    }
    d.headlineMetrics = {QStringLiteral("score.3d"), QStringLiteral("score.2d")};
    d.headlineLabels.insert(QStringLiteral("score.3d"), QStringLiteral("3D"));
    d.headlineLabels.insert(QStringLiteral("score.2d"), QStringLiteral("2D"));

    const QVector<ApiId> apis = shownApis(r);
    const QString gpuName = text(r, QStringLiteral("gpu.name"));
    const QString renderSize = text(r, QStringLiteral("render.size"));
    const auto scale = number(r, QStringLiteral("render.scale"));
    const bool fit = text(r, QStringLiteral("window.size")).contains(QStringLiteral("scaled to fit"));
    const bool scored = r.status == Status::Ok || r.status == Status::Partial;
    const QString scaleText = scale ? QStringLiteral("%1%").arg(std::llround(*scale)) : QString();
    const Failure failure = failureOf(r.statusReason);

    // Subtitle: "Score 1,254 · AMD Radeon RX 7800 XT · 1920×1080 at 100% · 4 APIs"; a Partial one says why it has no score.
    QStringList parts;
    if (r.score) {
        parts << QStringLiteral("Score %1").arg(Units::format(*r.score, QStringLiteral("points"), false));
    } else if (r.status == Status::Partial) {
        QString why = QStringLiteral("No GPU score");
        if (failure.matched) {
            const QString what = failure.drew() ? failure.why.section(QStringLiteral(" ("), 0, 0) : QStringLiteral("failed during ") + failure.workload;
            why += QStringLiteral(" · %1 %2").arg(failure.api, what);
        }
        parts << why;
    } else if (r.status == Status::Cancelled) {
        parts << QStringLiteral("Stopped");
    }
    if (!gpuName.isEmpty())
        parts << gpuName;
    if (!renderSize.isEmpty())
        parts << (scaleText.isEmpty() ? renderSize : QStringLiteral("%1 at %2").arg(renderSize, scaleText));
    if (!apis.isEmpty())
        parts << (apis.size() == 1 ? QStringLiteral("1 API") : QStringLiteral("%1 APIs").arg(apis.size()));
    d.subtitle = parts.join(QStringLiteral(" · "));

    // 1. KPIs: GPU score · 3D · 2D · Render · GPU (M4).
    const QString renderUnder =
        scaleText.isEmpty() ? QString() : QStringLiteral("at %1 · %2").arg(scaleText, fit ? QStringLiteral("scaled to fit") : QStringLiteral("fits"));
    KpisBlock kpis;
    if (r.score)
        kpis.items.append({QStringLiteral("GPU score"), DetailCell::literal(Units::format(*r.score, QStringLiteral("points"), false)),
                           QStringLiteral("points")});
    if (r.metric(QStringLiteral("score.3d")))
        kpis.items.append({QStringLiteral("3D"), DetailCell::metric(QStringLiteral("score.3d")), QStringLiteral("best API per scene")});
    if (r.metric(QStringLiteral("score.2d")))
        kpis.items.append({QStringLiteral("2D"), DetailCell::metric(QStringLiteral("score.2d")), QStringLiteral("native 50% · fill-rate 50%")});
    if (!renderSize.isEmpty())
        kpis.items.append({QStringLiteral("Render"), DetailCell::literal(renderSize), renderUnder});
    if (!gpuName.isEmpty()) {
        QStringList under;
        for (const char *k : {"gpu.vram", "gpu.kind"}) {
            if (const QString t = text(r, QLatin1StringView(k)); !t.isEmpty())
                under << t;
        }
        kpis.items.append({QStringLiteral("GPU"), DetailCell::literal(shortGpuName(gpuName)), under.join(QStringLiteral(" · "))});
    }
    if (!kpis.items.isEmpty())
        d.blocks.append({{}, kpis});

    // 2. Sub-tests (Group | Weight | Result | Sub-score) and its summary.
    {
        TableBlock groups = table({QStringLiteral("Group"), QStringLiteral("Weight"), QStringLiteral("Result")});
        if (scored)
            groups.columns << QStringLiteral("Sub-score");
        const auto row = [&](const QString &id, const QString &name, const char *weight, const QString &result) {
            const SubResult *s = sub(r, id);
            const bool ran = s && s->status == Status::Ok && !result.isEmpty();
            QVector<DetailCell> cells{DetailCell::literal(name), DetailCell::literal(QLatin1StringView(weight)),
                                      DetailCell::literal(ran ? result : statusText(s))};
            if (scored) {
                const QString score = subScore(s);
                cells << DetailCell::literal(score.isEmpty() ? kDash : score);
            }
            groups.rows.append(cells);
        };
        QStringList scenes;
        for (const SceneId s : kScenes) {
            if (const QString v = value(r, sceneBestKey(s, u"fps")); !v.isEmpty())
                scenes << QStringLiteral("%1 %2").arg(sceneLabel(s), v);
        }
        row(kScenes3d, QStringLiteral("3D scenes"), "80%",
            scenes.size() == int(kScenes.size()) ? scenes.join(QStringLiteral(" · ")) + QStringLiteral(" fps") : QString());
        QStringList natives;
        for (const NativeWorkload w : kNativeWorkloads) {
            if (const QString v = value(r, nativeBestKey(w)); !v.isEmpty())
                natives << QStringLiteral("%1 %2").arg(w == NativeWorkload::Blits ? QStringLiteral("Blits") : nativeName(w), v);
        }
        row(kNative2d, QStringLiteral("Native 2D (Qt)"), "10%",
            natives.size() == int(kNativeWorkloads.size()) ? natives.join(QStringLiteral(" · ")) + QStringLiteral(" MOps/s") : QString());
        const QString quads = value(r, fillBestKey(FillId::Quads)), sprites = value(r, fillBestKey(FillId::Sprites));
        row(kFill2d, QStringLiteral("2D fill-rate"), "10%",
            !quads.isEmpty() && !sprites.isEmpty() ? QStringLiteral("Quads %1 Gpx/s · Sprites %2 MOps/s").arg(quads, sprites) : QString());
        if (scored) {
            groups.boldRows.append(int(groups.rows.size()));
            groups.rows.append({DetailCell::literal(QStringLiteral("GPU score")), DetailCell::literal(QStringLiteral("100%")),
                                DetailCell::literal(r.score ? QStringLiteral("weighted geometric mean") : QStringLiteral("no score: not every part ran")),
                                DetailCell::literal(r.score ? Units::format(*r.score, QStringLiteral("points"), false) : kDash)});
        }
        const auto part = [&](const QString &label, const QString &id) {
            const QString s = subScore(sub(r, id));
            return QStringLiteral("%1 %2").arg(label, s.isEmpty() ? kDash : s);
        };
        groups.summary = QStringList{part(QStringLiteral("3D"), kScenes3d), part(QStringLiteral("Native 2D"), kNative2d),
                                     part(QStringLiteral("Fill-rate"), kFill2d)}
                             .join(QStringLiteral(" · "));
        d.blocks.append({QStringLiteral("Sub-tests"), groups});
    }

    // 3. "3D scenes: average FPS by API": the best value of each row is bold, N/A APIs have no column; then its note.
    const auto bestApi = [&](SceneId s) { return apiOfName(text(r, sceneBestKey(s, u"api"))); };
    bool anyScene = false;
    for (const SceneId s : kScenes) {
        for (const ApiId a : apis)
            anyScene = anyScene || r.metric(sceneApiKey(a, s, u"fps"));
    }
    if (anyScene) {
        TableBlock t = table({QStringLiteral("Scene")});
        for (const ApiId a : apis)
            t.columns << apiShort(a);
        QVector<int> wins(apis.size(), 0);
        for (const SceneId s : kScenes) {
            QVector<DetailCell> cells{DetailCell::literal(sceneLabel(s))};
            for (const ApiId a : apis) {
                QString cell = valueOrDash(r, sceneApiKey(a, s, u"fps"));
                if (cell == kDash)
                    cell = missingCell(r, a, sceneName(s));
                cells << DetailCell::literal(cell);
            }
            if (const auto best = bestApi(s); best && apis.contains(*best)) {
                const int at = int(apis.indexOf(*best));
                t.boldCells.append(QPoint(1 + at, int(t.rows.size())));
                ++wins[at];
            }
            t.rows.append(cells);
        }
        QStringList best;
        for (int i = 0; i < apis.size(); ++i) {
            if (wins[i] > 0)
                best << QStringLiteral("%1 %2").arg(apiShort(apis[i]), plural(wins[i], QStringLiteral("scene")));
        }
        t.summary = best.isEmpty() ? QString() : QStringLiteral("Best: ") + best.join(QStringLiteral(" · "));
        d.blocks.append({QStringLiteral("3D scenes: average FPS by API"), t});
        d.blocks.append({{}, NoteBlock{QStringLiteral("Each scene ran 20 s per API%1, scene by scene. The best API's average (bold) counts toward "
                                                      "the score; every API is shown.")
                                           .arg(renderSize.isEmpty() ? QString() : QStringLiteral(" at ") + renderSize),
                                       Tone::Neutral}});
    }

    // 4. Per scene, in order: the FPS chart (the best API), "<Scene> by API"; the Draw calls note follows its table.
    for (const SceneId scene : kScenes) {
        const auto best = bestApi(scene);
        if (best) {
            if (const QString key = seriesKey(apiKey(*best), sceneKey(scene)); hasSeries(r, key)) {
                LineChartBlock chart;
                chart.seriesKeys = {key};
                chart.yValue = ChartValue::PerSecondFromMs;
                chart.yUnit = QStringLiteral("fps");
                chart.xAxis.tickLabels = {QStringLiteral("0 s"), QStringLiteral("5 s"), QStringLiteral("10 s"), QStringLiteral("15 s"),
                                          QStringLiteral("20 s")};
                chart.xAxis.tickValues = {0, 5, 10, 15, 20};
                d.blocks.append({QStringLiteral("%1: FPS over time (%2, best API)").arg(sceneLabel(scene), apiShort(*best)), chart});
            }
        }
        bool any = false;
        for (const ApiId a : apis)
            any = any || r.metric(sceneApiKey(a, scene, u"fps"));
        if (!any)
            continue;
        TableBlock t = table({QStringLiteral("API"), QStringLiteral("Avg fps"), QStringLiteral("1% low"), QStringLiteral("GPU ms"), QStringLiteral("CPU ms")});
        for (const ApiId a : apis) {
            const bool missing = !r.metric(sceneApiKey(a, scene, u"fps"));
            if (best && a == *best)
                t.boldCells.append(QPoint(1, int(t.rows.size())));
            t.rows.append({DetailCell::literal(apiShort(a)),
                           DetailCell::literal(missing ? missingCell(r, a, sceneName(scene)) : valueOrDash(r, sceneApiKey(a, scene, u"fps"))),
                           DetailCell::literal(valueOrDash(r, sceneApiKey(a, scene, u"low"))), DetailCell::literal(ms(r, sceneApiKey(a, scene, u"gpu"))),
                           DetailCell::literal(ms(r, sceneApiKey(a, scene, u"cpu")))});
        }
        if (best) {
            QStringList summary;
            if (const QString avg = value(r, sceneBestKey(scene, u"fps")); !avg.isEmpty())
                summary << QStringLiteral("%1 %2 fps").arg(apiShort(*best), avg);
            if (const QString low = value(r, sceneBestKey(scene, u"low")); !low.isEmpty())
                summary << QStringLiteral("1% low %1").arg(low);
            t.summary = summary.join(QStringLiteral(" · "));
        }
        d.blocks.append({QStringLiteral("%1 by API").arg(sceneLabel(scene)), t});
        if (scene == SceneId::DrawCalls) {
            // 38,401 = 38,400 objects and the ground plane (design §6.4).
            QString detail;
            double lo = INFINITY, hi = 0;
            for (const ApiId a : apis) {
                if (const auto cpu = number(r, sceneApiKey(a, scene, u"cpu"))) {
                    lo = std::min(lo, *cpu);
                    hi = std::max(hi, *cpu);
                }
            }
            const auto gpuMs = best ? number(r, sceneApiKey(*best, scene, u"gpu")) : std::nullopt;
            if (gpuMs && std::isfinite(lo)) {
                const QString cpu = std::llround(lo) == std::llround(hi) ? QString::number(std::llround(hi))
                                                                          : QStringLiteral("%1–%2").arg(std::llround(lo)).arg(std::llround(hi));
                detail = QStringLiteral(" (GPU time about %1 ms against %2 ms of CPU time)").arg(*gpuMs, 0, 'f', 1).arg(cpu);
            }
            d.blocks.append({{}, NoteBlock{QStringLiteral("Draw calls measures the CPU cost of submitting 38,401 draws per frame%1, so a faster CPU raises it.")
                                               .arg(detail),
                                           Tone::Neutral}});
        }
    }

    // 5. "2D fill-rate" (API | Quads Gpx/s | Sprites MOps/s, the best of each column bold) and its note.
    bool anyFill = false;
    for (const ApiId a : apis)
        anyFill = anyFill || r.metric(fillApiKey(a, FillId::Quads)) || r.metric(fillApiKey(a, FillId::Sprites));
    if (anyFill) {
        TableBlock t = table({QStringLiteral("API"), QStringLiteral("Quads Gpx/s"), QStringLiteral("Sprites MOps/s")});
        const QString bestQuads = text(r, fillBestKey(FillId::Quads, true)), bestSprites = text(r, fillBestKey(FillId::Sprites, true));
        for (const ApiId a : apis) {
            const auto cell = [&](FillId f) {
                const QString v = valueOrDash(r, fillApiKey(a, f));
                return DetailCell::literal(v == kDash ? missingCell(r, a, fillName(f)) : v);
            };
            if (apiName(a) == bestQuads)
                t.boldCells.append(QPoint(1, int(t.rows.size())));
            if (apiName(a) == bestSprites)
                t.boldCells.append(QPoint(2, int(t.rows.size())));
            t.rows.append({DetailCell::literal(apiShort(a)), cell(FillId::Quads), cell(FillId::Sprites)});
        }
        QStringList summary;
        if (const QString q = value(r, fillBestKey(FillId::Quads)); !q.isEmpty())
            summary << QStringLiteral("Quads %1 Gpx/s (%2)").arg(q, shortOf(bestQuads));
        if (const QString s = value(r, fillBestKey(FillId::Sprites)); !s.isEmpty())
            summary << QStringLiteral("Sprites %1 MOps/s (%2)").arg(s, shortOf(bestSprites));
        t.summary = summary.join(QStringLiteral(" · "));
        d.blocks.append({QStringLiteral("2D fill-rate"), t});
        d.blocks.append({{}, NoteBlock{QStringLiteral("Fill-rate: %1 blended full-screen layers per frame, and %2 sprites streamed by the CPU every frame; "
                                                      "the best API (bold) counts.")
                                           .arg(kFillQuadLayers)
                                           .arg(QLocale(QLocale::English).toString(qlonglong(kFillSprites))),
                                       Tone::Neutral}});
    }

    // 6. "Native 2D (MOps/s)": Workload | Direct2D | Qt raster (scored), the Qt raster column bold; then its note.
    {
        bool anyD2d = false, anyQt = false;
        for (const NativeWorkload w : kNativeWorkloads) {
            anyD2d = anyD2d || r.metric(nativePathMetricKey(NativePath::Direct2D, w));
            anyQt = anyQt || r.metric(nativePathMetricKey(NativePath::QtRaster, w));
        }
        if (anyD2d || anyQt) {
            TableBlock t = table({QStringLiteral("Workload")});
            if (anyD2d)
                t.columns << QStringLiteral("Direct2D");
            t.columns << QStringLiteral("Qt raster (scored)");
            const int qtColumn = int(t.columns.size()) - 1;
            for (const NativeWorkload w : kNativeWorkloads) {
                QVector<DetailCell> cells{DetailCell::literal(nativeName(w))};
                if (anyD2d)
                    cells << DetailCell::literal(valueOrDash(r, nativePathMetricKey(NativePath::Direct2D, w)));
                cells << DetailCell::literal(valueOrDash(r, nativePathMetricKey(NativePath::QtRaster, w)));
                if (r.metric(nativePathMetricKey(NativePath::QtRaster, w)))
                    t.boldCells.append(QPoint(qtColumn, int(t.rows.size())));
                t.rows.append(cells);
            }
            const QString nativeScore = subScore(sub(r, kNative2d));
            t.summary = nativeScore.isEmpty() ? QStringLiteral("Qt raster (scored)") : QStringLiteral("Qt raster (scored) · sub-score %1").arg(nativeScore);
            d.blocks.append({QStringLiteral("Native 2D (MOps/s)"), t});
            d.blocks.append({{}, NoteBlock{QStringLiteral("Native 2D: Qt raster draws on the CPU and is scored; Direct2D draws on the GPU and is shown "
                                                          "for comparison."),
                                           Tone::Neutral}});
        }
    }

    // 7. "APIs": a row for each of the four 3D APIs, Direct2D and Qt raster that has a status.
    {
        struct Row {
            QString key, name, fixedNote;
        };
        QVector<Row> rows;
        for (const ApiId a : kApis)
            rows.append({apiKey(a), apiName(a), {}});
        rows.append({nativePathKey(NativePath::Direct2D), nativePathName(NativePath::Direct2D), QStringLiteral("GPU path, not scored")});
        rows.append({nativePathKey(NativePath::QtRaster), nativePathName(NativePath::QtRaster), QStringLiteral("CPU path, scored")});
        TableBlock t = table({QStringLiteral("API"), QStringLiteral("Status"), QStringLiteral("Version"), QStringLiteral("Driver"),
                              QStringLiteral("Present"), QStringLiteral("Note")});
        // Text, left-aligned as the mockup draws it; Driver and Note wrap before API, Version and Present (Zeb D4).
        t.leftColumns = {1, 2, 3, 4, 5};
        t.wrapColumns = {3, 5};
        int ok = 0, error = 0, na = 0, stoppedRows = 0;
        for (const Row &row : std::as_const(rows)) {
            const QString status = apiStatus(r, row.key);
            if (status.isEmpty())
                continue;
            const QString word = statusWord(status);
            ok += word == QLatin1StringView("OK");
            error += word == QLatin1StringView("Error");
            na += word == QLatin1StringView("N/A");
            stoppedRows += word == QLatin1StringView("Stopped");
            const auto field = [&](QStringView f) {
                const QString v = text(r, apiStatusKey(row.key, f));
                return DetailCell::literal(v.isEmpty() ? kDash : v);
            };
            QString note;
            if (word == QLatin1StringView("N/A"))
                note = statusReason(status);
            else if (word == QLatin1StringView("Error"))
                note = errorNote(r, row.name, status);
            else
                note = text(r, apiStatusKey(row.key, u"note"));
            if (word == QLatin1StringView("OK") && !row.fixedNote.isEmpty())
                note = note.isEmpty() ? row.fixedNote : row.fixedNote + QStringLiteral(" · ") + note;
            t.rows.append({DetailCell::literal(row.name), DetailCell::literal(word), field(u"version"), field(u"driver"), field(u"present"),
                           DetailCell::literal(note.isEmpty() ? kDash : note)});
        }
        if (!t.rows.isEmpty()) {
            QStringList counts;
            if (ok)
                counts << QStringLiteral("%1 OK").arg(ok);
            if (error)
                counts << QStringLiteral("%1 Error").arg(error);
            if (na)
                counts << QStringLiteral("%1 N/A").arg(na);
            if (stoppedRows)
                counts << QStringLiteral("%1 Stopped").arg(stoppedRows);  // the counts add up to the rows (AP-5 L4)
            t.summary = counts.join(QStringLiteral(" · "));
            d.blocks.append({QStringLiteral("APIs"), t});
        }
    }

    // 8. "Render setup": Item | Value.
    {
        TableBlock t = table({QStringLiteral("Item"), QStringLiteral("Value")});
        t.leftColumns = {1};  // the mockup's left-aligned values
        t.wrapColumns = {1};
        const auto add = [&](const QString &item, const QString &v) {
            if (!v.isEmpty())
                t.rows.append({DetailCell::literal(item), DetailCell::literal(v)});
        };
        if (!gpuName.isEmpty()) {
            QStringList detail;
            for (const char *k : {"gpu.kind", "gpu.vram"}) {
                if (const QString v = text(r, QLatin1StringView(k)); !v.isEmpty())
                    detail << v;
            }
            // One set of parentheses (Zeb nit): a name that ends in its driver's tag ("… (RADV STRIX_HALO)") takes the details inside it.
            QString row = gpuName;
            if (!detail.isEmpty()) {
                if (row.endsWith(QLatin1Char(')')) && row.contains(QStringLiteral(" (")))
                    row = row.chopped(1) + QStringLiteral(", ") + detail.join(QStringLiteral(", ")) + QLatin1Char(')');
                else
                    row = QStringLiteral("%1 (%2)").arg(row, detail.join(QStringLiteral(", ")));
            }
            add(QStringLiteral("GPU"), row);
        }
        QString driver = text(r, QStringLiteral("gpu.driver"));  // the OS-reported one; the APIs' own strings are in the APIs table
        for (const ApiId a : kApis) {
            if (!driver.isEmpty())
                break;
            driver = text(r, apiStatusKey(apiKey(a), u"driver"));
            if (!driver.isEmpty())
                break;
        }
        add(QStringLiteral("Driver"), driver);
        add(QStringLiteral("Render size"), renderSize.isEmpty() ? QString() : renderSize + QStringLiteral(" pixels per frame"));
        add(QStringLiteral("Scale"), scaleText);
        add(QStringLiteral("Window"), text(r, QStringLiteral("window.size")));
        add(QStringLiteral("Screen"), text(r, QStringLiteral("screen")));
        {
            QString session = text(r, QStringLiteral("session"));
            if (const QString hags = text(r, QStringLiteral("gpu.hags")); !hags.isEmpty())
                session += (session.isEmpty() ? QString() : QStringLiteral(" \u00b7 ")) + QStringLiteral("hardware-accelerated GPU scheduling ") + hags;
            add(QStringLiteral("Session"), session);
        }
        add(QStringLiteral("Shaders"), text(r, QStringLiteral("shaders")));
        QStringList run;
        if (r.durationMs > 0)
            run << duration(r.durationMs);
        if (const auto load = number(r, QStringLiteral("background.load")))
            run << QStringLiteral("background load %1%").arg(*load, 0, 'f', 1);
        if (const QString power = text(r, QStringLiteral("power.source")); !power.isEmpty())
            run << (power == QLatin1StringView("Battery") ? QStringLiteral("on battery") : QStringLiteral("on AC"));
        add(QStringLiteral("Run"), run.join(QStringLiteral(" · ")));
        if (!t.rows.isEmpty()) {
            t.summary = renderSize.isEmpty() ? QString() : (renderUnder.isEmpty() ? renderSize : renderSize + QLatin1Char(' ') + renderUnder);
            d.blocks.append({QStringLiteral("Render setup"), t});
        }
    }

    // 9. Notes about the run (Warn: the renderer draws them at the top). The scaled-to-fit note changes what the FPS
    // means, so it comes first.
    if (scale && !renderSize.isEmpty() && std::llround(*scale) != 100) {
        const QString at = QStringLiteral("%1%").arg(std::llround(*scale));
        d.blocks.append({{}, NoteBlock{fit ? QStringLiteral("Scaled to fit this screen at %1: %2 rendered per frame, so FPS isn't comparable with 100%.")
                                                 .arg(at, renderSize)
                                           : QStringLiteral("Rendered at %1 (%2 per frame), so FPS isn't comparable with 100%.").arg(at, renderSize),
                                       Tone::Warn}});
    }
    // Scaled to fit at 100% (Dave, AP-5 M1): no note. The render size is unchanged (only the present pass scales down), so
    // the FPS is comparable; the Render KPI already says "scaled to fit".
    if (r.status == Status::Partial) {
        QString note;
        if (failure.matched && failure.drew())
            note = QStringLiteral("No GPU score: %1 %2.").arg(failure.api, failure.why);
        else if (failure.matched)
            note = QStringLiteral("No GPU score: %1 failed during %2%3. %4")
                       .arg(failure.api, failure.workload, becauseOf(failure.why),
                            failure.workload == QLatin1StringView("start-up") ? QStringLiteral("Every other API is shown.")  // it ran nothing (L2)
                                                                              : QStringLiteral("Its other results and every other API are shown."));
        else
            note = r.statusReason.startsWith(QStringLiteral("No GPU score")) ? r.statusReason : QStringLiteral("No GPU score: %1").arg(r.statusReason);
        d.blocks.append({{}, NoteBlock{note, Tone::Warn}});
    } else if (r.status == Status::Error) {
        // AP-5 L5: an Error run says why at the top (nothing else on the page does).
        QString note;
        if (failure.matched && failure.drew())
            note = QStringLiteral("No GPU score: %1 %2.").arg(failure.api, failure.why);
        else if (failure.matched)
            note = QStringLiteral("No GPU score: %1 failed during %2%3.").arg(failure.api, failure.workload, becauseOf(failure.why));
        else
            note = QStringLiteral("No GPU score: %1.").arg(r.statusReason.isEmpty() ? QStringLiteral("the test failed") : r.statusReason);
        d.blocks.append({{}, NoteBlock{note, Tone::Warn}});
    } else if (r.status == Status::Cancelled) {
        d.blocks.append({{}, NoteBlock{r.metrics.isEmpty() ? QStringLiteral("Stopped before anything was measured.")
                                                           : QStringLiteral("Stopped before the end: the parts that finished are shown, without scores."),
                                       Tone::Warn}});
    } else if (r.status == Status::TimedOut || r.status == Status::Interrupted) {
        // AP-5 L1: with nothing measured there are no parts to show, so the reason alone is the note.
        const bool nothing = r.metrics.isEmpty();  // gpu.name is a metric, so no metrics means no name either
        d.blocks.append({{}, NoteBlock{nothing && !r.statusReason.isEmpty()
                                           ? QStringLiteral("No GPU score: %1.").arg(r.statusReason)
                                           : QStringLiteral("The test didn't finish: the parts that finished are shown, without scores."),
                                       Tone::Warn}});
    }
    if (errored(r, nativePathKey(NativePath::Direct2D))) {
        // §15's sentence only (AP-5 L6): the stored note also holds the ready note and the worker's warnings, joined by " · ".
        const QString n = text(r, apiStatusKey(nativePathKey(NativePath::Direct2D), u"note"));
        const qsizetype at = n.indexOf(QStringLiteral("Direct2D failed during "));
        if (at >= 0) {
            // The sentence ends at its fixed tail: the reason inside it may hold " · " itself.
            const QString tail = QStringLiteral("It isn't scored, so the GPU score stands.");
            const qsizetype end = n.indexOf(tail, at);
            d.blocks.append({{}, NoteBlock{end >= 0 ? n.mid(at, end + tail.size() - at) : n.mid(at).section(QStringLiteral(" \u00b7 "), 0, 0), Tone::Warn}});
        }
    }
    for (const ApiId a : apis) {
        if (const QString layers = text(r, apiStatusKey(apiKey(a), u"layers")); !layers.isEmpty())
            d.blocks.append({{}, NoteBlock{QStringLiteral("%1 layers were active (%2): overlays and recorders can lower the result.").arg(apiName(a), layers),
                                           Tone::Warn}});
    }
    if (const QString shownBy = text(r, QStringLiteral("gpu.display")); !shownBy.isEmpty() && !gpuName.isEmpty())
        d.blocks.append({{}, NoteBlock{QStringLiteral("Rendered on the %1; the window is shown by another GPU, so a cross-GPU copy is included.")
                                           .arg(shortGpuName(gpuName)),
                                       Tone::Warn}});
    if (text(r, QStringLiteral("power.source")) == QLatin1StringView("Battery"))
        d.blocks.append({{}, NoteBlock{QStringLiteral("On battery"), Tone::Warn}});
    if (const auto load = number(r, QStringLiteral("background.load")); load && *load >= 10)
        d.blocks.append({{}, NoteBlock{QStringLiteral("Background load %1% before the run").arg(std::lround(*load)), Tone::Warn}});
    return d;
}

} // namespace sb::tests
