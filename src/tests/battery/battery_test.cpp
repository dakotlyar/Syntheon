// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/battery/battery_test.h"

#include "tests/common/stub_clock.h"

#include <QCborMap>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sb::tests {

namespace {

constexpr int kSamples = 24;

double profileMinutes(const QString &profile)
{
    return profile == QLatin1StringView("light") ? 560.0 : profile == QLatin1StringView("heavy") ? 250.0 : 402.0;
}

// The drain curve and result metrics from (minutes, charge %) samples.
void fillResult(TestResult &r, const QVector<QPointF> &samples, double healthPct, double designWh)
{
    const double minutes = samples.isEmpty() ? 0 : samples.last().x();
    r.metrics.append(numberMetric(QStringLiteral("runtime"), QStringLiteral("Battery life"), minutes, QStringLiteral("min"),
                                  Direction::HigherIsBetter, true));
    const double usedWh = designWh * healthPct / 100.0;
    r.metrics.append(numberMetric(QStringLiteral("avg_drain"), QStringLiteral("Average drain"),
                                  minutes > 0 ? usedWh / (minutes / 60.0) : 0, QStringLiteral("W"), Direction::LowerIsBetter, true));
    r.metrics.append(numberMetric(QStringLiteral("health"), QStringLiteral("Battery health"), healthPct, QStringLiteral("%"),
                                  Direction::Neutral, true));
    r.metrics.append(numberMetric(QStringLiteral("full_wh"), QStringLiteral("Full-charge capacity"), usedWh, QStringLiteral("Wh")));
    Series drain{QStringLiteral("drain"), QStringLiteral("Charge over time"), QStringLiteral("Time"), QStringLiteral("min"),
                 QStringLiteral("%"), samples};
    r.series.append(drain);
    r.status = Status::Ok;
}

StubSpec makeSpec()
{
    StubSpec s;
    TestInfo &i = s.info;
    i.id = QStringLiteral("battery");
    i.name = QStringLiteral("Battery life");
    i.summary = QStringLiteral("Runs alone, looping tests until the battery is empty");
    i.colorToken = QStringLiteral("t-bat");
    i.exclusivity = Exclusivity::RunsAlone;
    i.recovery = RecoveryMode::FromCheckpoints;
    // Whether Battery counts toward the overall score is decided in Phase 8
    // (placeholder, confirmed by Dave 2026-10-15).
    i.countsTowardOverall = false;
    s.estimate = std::chrono::seconds(30);
    s.targetScore = 900;
    s.metrics.append({QStringLiteral("runtime"), {}, QStringLiteral("Battery life"), QStringLiteral("min"), 402, Direction::HigherIsBetter,
                      true, 1.0, {}});

    s.options = [](const ProbeContext &, const Availability &) {
        OptionSchema o;
        OptionDef profile;
        profile.key = QStringLiteral("profile");
        profile.label = QStringLiteral("Profile");
        profile.kind = OptionKind::SingleSelect;
        profile.presentation = SelectPresentation::Segmented;
        profile.items = {{QStringLiteral("light"), QStringLiteral("Light")},
                         {QStringLiteral("mixed"), QStringLiteral("Mixed")},
                         {QStringLiteral("heavy"), QStringLiteral("Heavy")}};
        profile.defaultValue = QStringLiteral("mixed");
        o.options.append(profile);
        OptionDef override;
        override.key = QStringLiteral("override_preconditions");
        override.label = QStringLiteral("Ignore the checks below (Phase 1 only)");
        override.kind = OptionKind::Bool;
        override.defaultValue = false;
        override.help = QStringLiteral("Lets you try the Battery stub on a desktop or while plugged in.");
        o.options.append(override);
        OptionDef note;
        note.key = QStringLiteral("note");
        note.label = QStringLiteral("Note");
        note.kind = OptionKind::Info;
        note.infoValue = QStringLiteral("Runs on its own until the machine shuts down; recovered next time SynBench opens.");
        o.options.append(note);
        return o;
    };

    s.preconditions = [](const OptionValues &opts, const ProbeContext &ctx) {
        const bool override = opts.value(QStringLiteral("override_preconditions"), false).toBool();
        const QJsonObject b = ctx.system().data.value(QStringLiteral("battery")).toObject();
        const bool present = b.value(QStringLiteral("present")).toBool();
        const bool ac = b.value(QStringLiteral("ac_online")).toBool();
        const double charge = b.value(QStringLiteral("charge_pct")).toDouble(-1);

        QList<Precondition> list;
        const auto add = [&](const QString &id, const QString &label, bool met, const QString &detail) {
            Precondition p;
            p.id = id;
            p.label = label;
            if (met) {
                p.state = Precondition::State::Met;
                p.detail = detail;
            } else if (override) {
                p.state = Precondition::State::Met;
                p.detail = QStringLiteral("Ignored (override)");
            } else {
                p.state = Precondition::State::NotMet;
                p.detail = detail;
            }
            list.append(p);
        };
        add(QStringLiteral("battery"), QStringLiteral("A battery is present"), present,
            present ? QStringLiteral("Found") : QStringLiteral("No battery found"));
        add(QStringLiteral("unplugged"), QStringLiteral("Running on battery"), present && !ac,
            ac ? QStringLiteral("On AC: unplug") : QStringLiteral("On battery"));
        add(QStringLiteral("charged"), QStringLiteral("Charged to 100%"), charge >= 99.5,
            charge >= 0 ? QStringLiteral("%1%").arg(qRound(charge)) : QStringLiteral("Unknown"));
        list.append({QStringLiteral("brightness"), QStringLiteral("Screen brightness locked"), Precondition::State::WillApplyAtStart,
                     QStringLiteral("Will lock at start")});
        list.append({QStringLiteral("sleep"), QStringLiteral("Sleep and screen-off blocked"), Precondition::State::WillApplyAtStart,
                     QStringLiteral("Will block at start")});
        return list;
    };

    s.customRun = [](RunContext &ctx, const OptionValues &o, std::mt19937 &rng) {
        const QString profile = o.value(QStringLiteral("profile"), QStringLiteral("mixed")).toString();
        const QString mode = o.value(QString(StubTest::kSimulateKey), QStringLiteral("normal")).toString();
        const double minutes = vary(rng, profileMinutes(profile));
        const double stepMs = 30000.0 * stubTimeScale() / kSamples;
        QVector<QPointF> samples;
        ctx.log(LogLevel::Info, QStringLiteral("Battery: %1 profile; display brightness locked and sleep blocked (simulated)").arg(profile));
        for (int n = 0; n <= kSamples; ++n) {
            const double t = minutes * n / kSamples;
            const double charge = 100.0 * (1.0 - double(n) / kSamples * (n % 6 < 4 ? 1.02 : 0.97));
            samples.append({t, std::max(0.0, charge)});
            // Durable: a power cut loses at most the sample being written.
            ctx.checkpoint(QCborMap{{QStringLiteral("t"), t}, {QStringLiteral("charge"), std::max(0.0, charge)}});
            ctx.progress(double(n) / kSamples, QStringLiteral("Draining · %1%").arg(qRound(std::max(0.0, charge))));
            if (mode == QLatin1StringView("hang") && n == kSamples / 2) {
                ctx.log(LogLevel::Warning, QStringLiteral("Simulated hang: ignoring Stop from here"));
                stubHang();
                return TestResult::error(QStringLiteral("Simulated hang ended"));
            }
            if (mode == QLatin1StringView("error") && n == kSamples / 2) {
                ctx.log(LogLevel::Error, QStringLiteral("Simulated error"));
                return TestResult::error(QStringLiteral("Simulated error while draining"));
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(qint64(stepMs * 1000.0));
            for (;;) {
                if (ctx.cancelled())
                    return TestResult::withStatus(Status::Cancelled);
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0)
                    break;
                QThread::msleep(std::min<unsigned long>(20, static_cast<unsigned long>(left.count())));
            }
        }
        TestResult r;
        const double health = snapshotNumber(ctx.system(), "battery", "health_pct", 94.0);
        const double design = snapshotNumber(ctx.system(), "battery", "design_wh", 90.0);
        fillResult(r, samples, health > 0 ? health : 94.0, design > 0 ? design : 90.0);
        r.durationMs = qint64(minutes * 60000.0);  // the simulated battery life
        return r;
    };

    s.recover = [](RecoveryContext &ctx) {
        QVector<QPointF> samples;
        for (const QCborMap &c : ctx.checkpoints())
            samples.append({c.value(QStringLiteral("t")).toDouble(), c.value(QStringLiteral("charge")).toDouble()});
        if (samples.isEmpty())
            return TestResult::interrupted(QStringLiteral("SynBench closed before the first sample"));
        TestResult r;
        const double health = snapshotNumber(ctx.system(), "battery", "health_pct", 94.0);
        const double design = snapshotNumber(ctx.system(), "battery", "design_wh", 90.0);
        fillResult(r, samples, health > 0 ? health : 94.0, design > 0 ? design : 90.0);
        r.statusReason = QStringLiteral("Recovered from checkpoints");
        r.durationMs = qint64(samples.last().x() * 60000.0);
        return r;
    };

    s.detail = [](const TestResult &r) {
        DetailLayout d;
        const QString profile = r.options.value(QStringLiteral("profile"), QStringLiteral("mixed")).toString();
        d.subtitle = scoreSubtitle(r, {QStringLiteral("%1 profile").arg(profile.left(1).toUpper() + profile.mid(1))});
        d.headlineMetrics = {QStringLiteral("runtime")};
        KpisBlock kpis;
        kpis.items = {{QStringLiteral("Battery life"), DetailCell::metric(QStringLiteral("runtime")), {}},
                      {QStringLiteral("Average drain"), DetailCell::metric(QStringLiteral("avg_drain")), {}},
                      {QStringLiteral("Battery health"), DetailCell::metric(QStringLiteral("health")), {}}};
        d.blocks.append({{}, kpis});
        if (!r.series.isEmpty()) {
            LineChartBlock chart;
            chart.seriesKeys = {QStringLiteral("drain")};
            chart.yUnit = QStringLiteral("%");
            chart.xAxis.scale = ChartAxis::Scale::Linear;
            const Metric *runtime = r.metric(QStringLiteral("runtime"));
            const double minutes = runtime && runtime->isNumeric() ? std::get<double>(runtime->value) : 480;
            for (int h = 0; h * 60 <= minutes + 30; ++h) {
                chart.xAxis.tickLabels.append(QStringLiteral("%1h").arg(h));
                chart.xAxis.tickValues.append(h * 60.0);  // minutes
            }
            d.blocks.append({QStringLiteral("Charge over time"), chart});
        }
        if (r.statusReason == QLatin1StringView("Recovered from checkpoints"))
            d.blocks.append({{}, NoteBlock{QStringLiteral("Recovered from checkpoints after the machine shut down."), Tone::Neutral}});
        return d;
    };
    return s;
}

} // namespace

BatteryTest::BatteryTest() : StubTest(makeSpec()) {}

} // namespace sb::tests
