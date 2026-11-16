// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU test's Detail page (Phase 4c design §12): the graphs-first layout
// with no chart (NPU has no frame-time curve / latency sweep), so — like
// Storage — "graphs first" is the KPI row + tables. Top to bottom: the KPI
// cards (NPU score + the three headline throughputs), the three-row Sub-tests
// weight + sub-score table with a bold total, a per-workload table (model +
// precision), the device table, and the notes. NotAvailable keeps NPU's own
// NaStateBlock (detail.h special-cases NPU, N9). Built only from the existing
// KpisBlock/TableBlock/NoteBlock/NaStateBlock.
#include "tests/npu/npu_test.h"

#include "core/units.h"
#include "tests/common/stub_test.h"
#include "tests/npu/formula_v1.h"
#include "tests/npu/ids.h"

#include <QStringList>

#include <cmath>

namespace sb::tests {

namespace {

const QString kDash = QStringLiteral("—");

const Metric *metric(const TestResult &r, const char *key)
{
    return r.metric(QLatin1StringView(key));
}

QString text(const TestResult &r, const char *key)
{
    const Metric *m = metric(r, key);
    if (!m || m->isNumeric())
        return {};
    return std::get<QString>(m->value);
}

const SubResult *sub(const TestResult &r, const QString &id)
{
    for (const SubResult &s : r.subResults)
        if (s.id == id)
            return &s;
    return nullptr;
}

QString subScore(const SubResult *s)
{
    return s && s->score ? Units::format(*s->score, QStringLiteral("points"), false) : kDash;
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

// The sub-test id ("classify") → the mockup's name ("Classify").
QString workloadName(const QString &id)
{
    if (const auto w = sb::npu::workloadFromId(QStringView{id}))
        return sb::npu::workloadLabel(*w);
    return id;
}

// A percentage for the weight line ("33.3%").
QString pct(double weight)
{
    QString s = QString::number(weight * 100.0, 'f', 1);
    if (s.endsWith(QLatin1StringView(".0")))
        s.chop(2);
    return s + QStringLiteral("%");
}

// "resnet50-int8 · INT8" → the model name / the precision.
QString modelName(const QString &modelText)
{
    return modelText.section(QStringLiteral(" · "), 0, 0);
}
QString modelPrecision(const QString &modelText)
{
    return modelText.section(QStringLiteral(" · "), 1, 1);
}

// The model.* key for one workload (classify/detect/llm).
const char *modelKey(sb::npu::Workload w)
{
    if (w == sb::npu::Workload::Detect)
        return "model.detect";
    if (w == sb::npu::Workload::Llm)
        return "model.llm";
    return "model.classify";
}

// The formatted headline value (e.g. "1,450 img/s"), or a dash.
QString formattedNumber(const Metric *m)
{
    if (!m || !m->isNumeric())
        return kDash;
    return Units::format(std::get<double>(m->value), m->unit, false) + QLatin1Char(' ') + m->unit;
}

} // namespace

DetailLayout NpuTest::detail(const TestResult &r) const
{
    // N/A state (§12 / N9): NPU keeps its own NaStateBlock; detail.h then keeps it.
    if (r.status == Status::NotAvailable) {
        DetailLayout d;
        d.subtitle = QStringLiteral("Not available on this machine");
        const QString why = r.statusReason.isEmpty() ? QStringLiteral("No supported NPU found") : r.statusReason;
        d.blocks.append({{}, NaStateBlock{QStringLiteral("No supported NPU found"), why}});
        return d;
    }

    DetailLayout d;
    const QString vendor = text(r, "device.vendor");
    const QString device = text(r, "device.name");
    const QString driver = text(r, "device.driver");
    const QString runtime = text(r, "device.runtime");
    const QString engine = text(r, "engine.name");
    const QString pack = text(r, "models.pack_version");
    const bool scored = r.status == Status::Ok || r.status == Status::Partial;

    // The three headline throughputs (§9) on the Results row, with short labels.
    d.headlineMetrics = {QStringLiteral("classify.imgs"), QStringLiteral("detect.imgs"), QStringLiteral("llm.tps")};
    d.headlineLabels.insert(QStringLiteral("classify.imgs"), QStringLiteral("Classify"));
    d.headlineLabels.insert(QStringLiteral("detect.imgs"), QStringLiteral("Detect"));
    d.headlineLabels.insert(QStringLiteral("llm.tps"), QStringLiteral("LLM"));

    // Subtitle (§12): "Score 1,000 · Intel AI Boost · OpenVINO NPU · INT8/INT4".
    QStringList parts;
    if (!device.isEmpty())
        parts << device;
    if (!engine.isEmpty())
        parts << engine;
    QStringList precs;
    for (const char *key : {"model.classify", "model.detect", "model.llm"}) {
        const QString p = modelPrecision(text(r, key));
        if (!p.isEmpty() && !precs.contains(p))
            precs << p;
    }
    if (!precs.isEmpty())
        parts << precs.join(QLatin1Char('/'));
    else if (device.isEmpty() && engine.isEmpty())
        parts << QStringLiteral("NPU");
    d.subtitle = scoreSubtitle(r, parts);

    // 1. KPI cards (§12): the score, then the three headline throughputs.
    KpisBlock kpis;
    if (r.score)
        kpis.items.append({QStringLiteral("NPU score"),
                           DetailCell::literal(Units::format(*r.score, QStringLiteral("points"), false)),
                           QStringLiteral("points")});
    const auto kpi = [&](const char *label, const char *key) {
        const Metric *m = metric(r, key);
        if (!m || !m->isNumeric())
            return;
        kpis.items.append({QLatin1StringView(label), DetailCell::literal(Units::format(std::get<double>(m->value), m->unit, false)),
                           m->unit});
    };
    kpi("Classification", "classify.imgs");
    kpi("Detection", "detect.imgs");
    kpi("LLM generation", "llm.tps");
    if (!kpis.items.isEmpty())
        d.blocks.append({{}, kpis});

    // 2. Sub-tests weight + sub-score table (§12, three rows + a bold total).
    TableBlock groups;
    groups.columns = {QStringLiteral("Workload"), QStringLiteral("Weight"), QStringLiteral("Result")};
    if (scored)
        groups.columns << QStringLiteral("Sub-score");
    for (const GeometricGroup &g : sb::npu::formula_v1::groups()) {
        const SubResult *s = sub(r, g.id);
        const bool ran = s && s->status == Status::Ok;
        const Metric *m = g.metrics.isEmpty() ? nullptr : metric(r, g.metrics.first().metricKey.toLatin1().constData());
        QVector<DetailCell> cells{DetailCell::literal(workloadName(g.id)), DetailCell::literal(pct(g.weight)),
                                  DetailCell::literal(ran ? formattedNumber(m) : statusText(s))};
        if (scored)
            cells << DetailCell::literal(subScore(s));
        groups.rows.append(cells);
    }
    if (scored) {
        groups.boldRows.append(int(groups.rows.size()));
        groups.rows.append({DetailCell::literal(QStringLiteral("NPU score")), DetailCell::literal(QStringLiteral("100%")),
                            DetailCell::literal(r.score ? QStringLiteral("weighted geometric mean")
                                                        : QStringLiteral("no score: not every workload ran")),
                            DetailCell::literal(r.score ? Units::format(*r.score, QStringLiteral("points"), false) : kDash)});
    }
    d.blocks.append({QStringLiteral("Sub-tests"), groups});

    // 3. Per-workload table (§12): model name + precision + the headline result.
    TableBlock per;
    per.fullWidth = true;
    per.columns = {QStringLiteral("Workload"), QStringLiteral("Model"), QStringLiteral("Precision"), QStringLiteral("Result")};
    for (sb::npu::Workload w : sb::npu::kWorkloads) {
        const QString model = text(r, modelKey(w));
        const Metric *m = metric(r, sb::npu::key(w).toLatin1().constData());
        per.rows.append({DetailCell::literal(sb::npu::workloadLabel(w)),
                         DetailCell::literal(modelName(model).isEmpty() ? kDash : modelName(model)),
                         DetailCell::literal(modelPrecision(model).isEmpty() ? kDash : modelPrecision(model)),
                         DetailCell::literal(formattedNumber(m))});
    }
    d.blocks.append({QStringLiteral("Workloads"), per});

    // 4. Device table (§12): vendor/NPU/driver/runtime + engine + model pack.
    TableBlock dev;
    dev.columns = {QStringLiteral("Field"), QStringLiteral("Value")};
    const auto field = [&](const QString &label, const QString &v) {
        dev.rows.append({DetailCell::literal(label), DetailCell::literal(v.isEmpty() ? kDash : v)});
    };
    field(QStringLiteral("Vendor"), vendor);
    field(QStringLiteral("NPU"), device);
    field(QStringLiteral("Driver"), driver);
    field(QStringLiteral("Runtime"), runtime);
    field(QStringLiteral("Engine"), engine);
    field(QStringLiteral("Model pack"), pack);
    d.blocks.append({QStringLiteral("Device"), dev});

    // 5. Notes (§12): the engine + the conditions.
    if (!engine.isEmpty())
        d.blocks.append({{}, NoteBlock{QStringLiteral("Dedicated NPU inference through %1. Each workload is the median of three "
                                                       "5-second windows; the first inference is verified against an expected output "
                                                       "shape before it is measured.")
                                            .arg(engine),
                                       Tone::Neutral}});
    if (r.status == Status::Partial) {
        d.blocks.append({{}, NoteBlock{QStringLiteral("No NPU score: not every workload ran."), Tone::Warn}});
    } else if (r.status == Status::Error) {
        d.blocks.append({{}, NoteBlock{QStringLiteral("No NPU score: %1.").arg(r.statusReason.isEmpty() ? QStringLiteral("the test failed") : r.statusReason),
                                       Tone::Warn}});
    } else if (r.status == Status::Cancelled) {
        d.blocks.append({{}, NoteBlock{QStringLiteral("Stopped before the end: the workloads that finished are shown, without scores."),
                                       Tone::Warn}});
    } else if (r.status == Status::TimedOut || r.status == Status::Interrupted) {
        d.blocks.append({{}, NoteBlock{QStringLiteral("The test didn't finish: the workloads that finished are shown, without scores."),
                                       Tone::Warn}});
    }
    if (text(r, "power.source") == QLatin1StringView("Battery"))
        d.blocks.append({{}, NoteBlock{QStringLiteral("On battery"), Tone::Warn}});

    return d;
}

} // namespace sb::tests
