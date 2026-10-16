// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/common/stub_test.h"

#include "core/units.h"
#include "tests/common/stub_clock.h"

#include <QThread>

#include <algorithm>
#include <cmath>

namespace sb::tests {

namespace {

constexpr int kSliceMs = 20;

std::uint32_t fnv1a(const QString &s)
{
    std::uint32_t h = 2166136261u;
    for (char c : s.toUtf8()) {
        h ^= std::uint8_t(c);
        h *= 16777619u;
    }
    return h;
}

QString simulateMode(const OptionValues &o)
{
    return o.value(QString(StubTest::kSimulateKey), QStringLiteral("normal")).toString();
}

// Sleeps `ms`, polling for cancel every slice. Returns false if cancelled.
bool sleepCancellable(RunContext &ctx, double ms)
{
    double left = ms;
    while (left > 0) {
        if (ctx.cancelled())
            return false;
        const int slice = std::max(1, int(std::min<double>(kSliceMs, std::ceil(left))));
        QThread::msleep(slice);
        left -= slice;
    }
    return !ctx.cancelled();
}

} // namespace

OptionDef simulateOption()
{
    OptionDef o;
    o.key = QString(StubTest::kSimulateKey);
    o.label = QStringLiteral("Simulate (Phase 1 only)");
    o.kind = OptionKind::SingleSelect;
    o.presentation = SelectPresentation::Segmented;
    o.items = {{QStringLiteral("normal"), QStringLiteral("Normal")},
               {QStringLiteral("error"), QStringLiteral("Error")},
               {QStringLiteral("na"), QStringLiteral("N/A")},
               {QStringLiteral("hang"), QStringLiteral("Hang")}};
    o.defaultValue = QStringLiteral("normal");
    o.help = QStringLiteral("Makes this stub fail, report N/A, or ignore Stop, to try those paths. "
                            "Removed when the real test arrives.");
    return o;
}

OptionDef seedOption()
{
    OptionDef o;
    o.key = QString(StubTest::kSeedKey);
    o.label = QStringLiteral("Variation seed (Phase 1 only)");
    o.kind = OptionKind::Number;
    o.minimum = 0;
    o.maximum = 9999;
    o.step = 1;
    o.defaultValue = 1.0;
    o.help = QStringLiteral("The same seed gives the same fake numbers.");
    return o;
}

double vary(std::mt19937 &rng, double base)
{
    std::uniform_real_distribution<double> u(-0.05, 0.05);
    return base * (1.0 + u(rng));
}

QString snapshotString(const SystemSnapshot &s, const char *object, const char *key)
{
    return s.data.value(QLatin1StringView(object)).toObject().value(QLatin1StringView(key)).toString();
}

double snapshotNumber(const SystemSnapshot &s, const char *object, const char *key, double fallback)
{
    const QJsonValue v = s.data.value(QLatin1StringView(object)).toObject().value(QLatin1StringView(key));
    return v.isDouble() ? v.toDouble() : fallback;
}

Metric textMetric(const QString &key, const QString &label, const QString &value, bool headline)
{
    return {key, {}, label, value, {}, Direction::Neutral, headline};
}

Metric numberMetric(const QString &key, const QString &label, double value, const QString &unit,
                    Direction direction, bool headline)
{
    return {key, {}, label, value, unit, direction, headline};
}

QString scoreSubtitle(const TestResult &r, const QStringList &parts)
{
    QStringList all;
    if (r.score)
        all << QStringLiteral("Score %1").arg(Units::format(*r.score, QStringLiteral("points"), false));
    all += parts;
    all.removeAll(QString());
    return all.join(QStringLiteral(" · "));
}

QString metricText(const TestResult &r, const QString &key)
{
    const Metric *m = r.metric(key);
    if (!m)
        return QStringLiteral("—");
    if (const auto *text = std::get_if<QString>(&m->value))
        return *text;
    return Units::format(std::get<double>(m->value), m->unit);
}

bool optionContains(const OptionValues &o, const QString &key, const QString &item)
{
    if (!o.contains(key))
        return true;
    const QVariant v = o.value(key);
    if (v.typeId() == QMetaType::QStringList)
        return v.toStringList().contains(item);
    for (const QVariant &e : v.toList()) {
        if (e.toString() == item)
            return true;
    }
    return false;
}

Availability StubTest::probe(const ProbeContext &ctx) const
{
    return m_spec.probe ? m_spec.probe(ctx) : Availability::available();
}

OptionSchema StubTest::options(const ProbeContext &ctx, const Availability &a) const
{
    OptionSchema s = m_spec.options ? m_spec.options(ctx, a) : OptionSchema{};
    if (a.state != Availability::State::Unavailable) {
        s.options.append(simulateOption());
        s.options.append(seedOption());
    }
    return s;
}

QList<Precondition> StubTest::preconditions(const OptionValues &o, const ProbeContext &ctx) const
{
    return m_spec.preconditions ? m_spec.preconditions(o, ctx) : QList<Precondition>{};
}

OptionValues StubTest::withDefaults(const OptionValues &o) const
{
    OptionValues v = o;
    if (!v.contains(QString(kSimulateKey)))
        v.insert(QString(kSimulateKey), QStringLiteral("normal"));
    if (!v.contains(QString(kSeedKey)))
        v.insert(QString(kSeedKey), 1.0);
    return v;
}

QStringList StubTest::selectedSubTests(const OptionValues &o) const
{
    QStringList all;
    for (const SubTestInfo &s : m_spec.info.subTests) {
        if (s.userSelectable)
            all.append(s.id);
    }
    if (all.isEmpty())
        return {};
    if (!o.contains(QString(kSubTestsOptionKey))) {
        QStringList defaults;
        for (const SubTestInfo &s : m_spec.info.subTests) {
            if (s.userSelectable && s.defaultOn)
                defaults.append(s.id);
        }
        return defaults;
    }
    const QVariant v = o.value(QString(kSubTestsOptionKey));
    QStringList chosen = v.typeId() == QMetaType::QStringList ? v.toStringList() : QStringList();
    if (v.typeId() == QMetaType::QVariantList) {
        for (const QVariant &e : v.toList())
            chosen.append(e.toString());
    }
    QStringList ordered;  // declaration order, whatever order they were ticked in
    for (const QString &id : all) {
        if (chosen.contains(id))
            ordered.append(id);
    }
    return ordered;
}

Validation StubTest::validate(const OptionValues &o) const
{
    Validation v;
    const QString mode = simulateMode(o);
    if (mode != QLatin1StringView("normal") && mode != QLatin1StringView("error") && mode != QLatin1StringView("na")
        && mode != QLatin1StringView("hang"))
        v.add(QString(kSimulateKey), QStringLiteral("Unknown simulate mode \"%1\"").arg(mode));
    bool hasSelectable = false;
    for (const SubTestInfo &s : m_spec.info.subTests)
        hasSelectable = hasSelectable || s.userSelectable;
    if (hasSelectable && selectedSubTests(o).isEmpty())
        v.add(QString(kSubTestsOptionKey), QStringLiteral("Select at least one sub-test"));
    return v;
}

std::chrono::seconds StubTest::estimate(const OptionValues &o) const
{
    double secs = double(m_spec.estimate.count());
    int total = 0;
    for (const SubTestInfo &s : m_spec.info.subTests)
        total += s.userSelectable ? 1 : 0;
    if (total > 0)
        secs *= std::max<double>(1, selectedSubTests(o).size()) / total;
    if (m_spec.estimateFactor)
        secs *= m_spec.estimateFactor(o);
    return std::chrono::seconds(std::max<long long>(m_spec.estimate.count() > 0 ? 1 : 0, std::llround(secs)));
}

std::chrono::seconds StubTest::timeLimit(const OptionValues &o) const
{
    if (m_spec.info.exclusivity == Exclusivity::RunsAlone)
        return std::chrono::seconds(0);  // runs until the battery is empty (Phase 8)
    const double secs = std::max(20.0, 2.0 * estimate(o).count()) * stubTimeScale();
    return std::chrono::seconds(std::max<long long>(1, std::llround(secs)));
}

TestResult StubTest::run(RunContext &ctx, const OptionValues &optionsIn)
{
    const OptionValues o = withDefaults(optionsIn);
    const QString mode = simulateMode(o);
    std::mt19937 rng(std::uint32_t(o.value(QString(kSeedKey)).toDouble()) * 7919u + fnv1a(m_spec.info.id));
    const SystemSnapshot &system = ctx.system();

    if (mode == QLatin1StringView("na")) {
        ctx.log(LogLevel::Info, QStringLiteral("Simulated: %1 is not available on this machine").arg(m_spec.info.name));
        return TestResult::notAvailable(QStringLiteral("Simulated: not available on this machine"));
    }

    TestResult r;
    if (m_spec.customRun) {
        r = m_spec.customRun(ctx, o, rng);
    } else {
        const QStringList subs = selectedSubTests(o);
        QStringList steps = subs;
        if (steps.isEmpty())
            steps = m_spec.stages.isEmpty() ? QStringList{QStringLiteral("Measuring")} : m_spec.stages;
        const double totalMs = double(estimate(o).count()) * 1000.0 * stubTimeScale();
        const double stepMs = totalMs / double(steps.size());

        for (int i = 0; i < steps.size(); ++i) {
            const bool isSub = !subs.isEmpty();
            QString name = steps[i];
            if (isSub) {
                for (const SubTestInfo &s : m_spec.info.subTests) {
                    if (s.id == steps[i])
                        name = s.name;
                }
                ctx.subTest(steps[i]).begin();
            }
            ctx.log(LogLevel::Info, QStringLiteral("%1: %2").arg(m_spec.info.name, name));
            ctx.progress(double(i) / double(steps.size()), name);

            const bool failHere = i == steps.size() / 2;
            if (mode == QLatin1StringView("hang") && failHere) {
                ctx.log(LogLevel::Warning, QStringLiteral("Simulated hang: ignoring Stop from here"));
                // Deliberately never looks at ctx.cancelled(): the orchestrator
                // must abandon this run (R7). Bounded so a stray process ends.
                QThread::sleep(15 * 60);
                return TestResult::error(QStringLiteral("Simulated hang ended"));
            }
            const int slices = std::max(1, int(stepMs / (kSliceMs * 5)));
            for (int s = 0; s < slices; ++s) {
                if (!sleepCancellable(ctx, stepMs / slices)) {
                    if (isSub)
                        ctx.subTest(steps[i]).end(Status::Cancelled);
                    return TestResult::withStatus(Status::Cancelled);
                }
                ctx.progress((double(i) + double(s + 1) / slices) / double(steps.size()), name);
            }
            if (mode == QLatin1StringView("error") && failHere) {
                ctx.log(LogLevel::Error, QStringLiteral("Simulated error while running %1").arg(name));
                if (isSub)
                    ctx.subTest(steps[i]).end(Status::Error);
                return TestResult::error(QStringLiteral("Simulated error while running %1").arg(name));
            }
            if (isSub)
                ctx.subTest(steps[i]).end(Status::Ok);
        }
        for (const QString &id : subs)
            r.subResults.append({id, Status::Ok, {}, {}});
        r.status = Status::Ok;
    }
    if (r.status == Status::Cancelled || r.status == Status::Error)
        return r;

    // A custom run (Battery) builds its own metrics; the table is then only
    // the scoring recipe.
    for (const StubMetricSpec &m : std::as_const(m_spec.metrics)) {
        if (m_spec.customRun)
            break;
        if (m.when && !m.when(o))
            continue;
        if (!m.subTestId.isEmpty() && !selectedSubTests(o).contains(m.subTestId))
            continue;
        r.metrics.append({m.key, m.subTestId, m.label, vary(rng, m.base), m.unit, m.direction, m.headline});
    }
    if (m_spec.extra)
        m_spec.extra(r, o, rng, system);
    ctx.progress(1.0, QStringLiteral("Done"));
    return r;
}

TestResult StubTest::recover(RecoveryContext &ctx)
{
    return m_spec.recover ? m_spec.recover(ctx) : ITest::recover(ctx);
}

ScoreBreakdown StubTest::score(const TestResult &r) const
{
    // The real formula shape, round(C x sum(w x m)), with made-up weights:
    // w = share x target / base, so every metric at its base gives the target.
    ScoreBreakdown b;
    double total = 0;
    bool any = false;
    QMap<QString, double> perSub;
    for (const Metric &m : r.metrics) {
        if (!m.isNumeric())
            continue;
        for (const StubMetricSpec &spec : m_spec.metrics) {
            if (spec.key != m.key || spec.share <= 0 || spec.base == 0)
                continue;
            const double part = spec.share * m_spec.targetScore / spec.base * std::get<double>(m.value);
            total += part;
            any = true;
            if (!spec.subTestId.isEmpty())
                perSub[spec.subTestId] += part;
        }
    }
    if (!any)
        return b;
    b.total = std::round(total);
    for (auto it = perSub.cbegin(); it != perSub.cend(); ++it)
        b.subScores.insert(it.key(), std::round(it.value()));
    return b;
}

DetailLayout StubTest::detail(const TestResult &r) const
{
    if (m_spec.detail && (r.status == Status::Ok || r.status == Status::Partial || r.status == Status::NotAvailable))
        return m_spec.detail(r);
    return genericDetailLayout(r);
}

} // namespace sb::tests
