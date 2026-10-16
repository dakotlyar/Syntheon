// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/itest.h"

#include <functional>
#include <random>

namespace sb::tests {

// Phase 1 stand-ins for the seven tests (D15): they implement the real
// contract with fake data so the GUI can be exercised end to end. Values are
// plausible (±5 % seeded variation around the mockup's examples), scores use
// the real formula shape round(C x sum(w x m)) with made-up weights, and a
// Phase-1-only "Simulate" option makes a run behave: normally, as an error,
// as N/A, or as a hang that ignores Stop. testVersion and formulaVersion are 0.
//
// Each phase replaces its folder under src/tests/ with the real test; nothing
// else changes.

struct StubMetricSpec {
    QString key, subTestId, label, unit;
    double base = 0;
    Direction direction = Direction::HigherIsBetter;
    bool headline = false;
    double share = 0;  // share of the stub score formula (0 = not scored)
    std::function<bool(const OptionValues &)> when;  // empty = always
};

struct StubSpec {
    TestInfo info;
    std::chrono::seconds estimate{20};
    double targetScore = 1000;  // the score the formula gives when every metric is at its base
    QVector<StubMetricSpec> metrics;
    QStringList stages;  // progress stages for a test without sub-tests

    // Test-specific options; the framework appends Simulate and the seed.
    std::function<OptionSchema(const ProbeContext &, const Availability &)> options;
    std::function<Availability(const ProbeContext &)> probe;
    std::function<QList<Precondition>(const OptionValues &, const ProbeContext &)> preconditions;
    // Text metrics, series, anything the table can't express.
    std::function<void(TestResult &, const OptionValues &, std::mt19937 &, const SystemSnapshot &)> extra;
    std::function<DetailLayout(const TestResult &)> detail;
    // Scales the estimate with the options (durations, selected sub-tests).
    std::function<double(const OptionValues &)> estimateFactor;
    // Battery: the run's body replaces the default timeline.
    std::function<TestResult(RunContext &, const OptionValues &, std::mt19937 &)> customRun;
    std::function<TestResult(RecoveryContext &)> recover;
};

class StubTest : public ITest
{
public:
    explicit StubTest(StubSpec spec) : m_spec(std::move(spec)) {}

    const TestInfo &info() const override { return m_spec.info; }
    Availability probe(const ProbeContext &ctx) const override;
    OptionSchema options(const ProbeContext &ctx, const Availability &a) const override;
    QList<Precondition> preconditions(const OptionValues &, const ProbeContext &) const override;
    Validation validate(const OptionValues &) const override;
    std::chrono::seconds estimate(const OptionValues &) const override;
    std::chrono::seconds timeLimit(const OptionValues &) const override;
    TestResult run(RunContext &, const OptionValues &) override;
    TestResult recover(RecoveryContext &) override;
    ScoreBreakdown score(const TestResult &) const override;
    DetailLayout detail(const TestResult &) const override;

    static constexpr QLatin1StringView kSimulateKey{"simulate"};
    static constexpr QLatin1StringView kSeedKey{"seed"};

private:
    QStringList selectedSubTests(const OptionValues &) const;
    OptionValues withDefaults(const OptionValues &) const;

    StubSpec m_spec;
};

// Helpers shared by the stub definitions.
OptionDef simulateOption();
OptionDef seedOption();
// base x (1 + u), u uniform in [-0.05, 0.05].
double vary(std::mt19937 &rng, double base);
// Reads a value from the system snapshot, e.g. snapshotString(s, "cpu", "name").
QString snapshotString(const SystemSnapshot &s, const char *object, const char *key);
double snapshotNumber(const SystemSnapshot &s, const char *object, const char *key, double fallback);
Metric textMetric(const QString &key, const QString &label, const QString &value, bool headline = false);
Metric numberMetric(const QString &key, const QString &label, double value, const QString &unit,
                    Direction direction = Direction::Neutral, bool headline = false);

// "Score 1,284 · AVX2 path · 16 threads" (or just the parts if unscored).
QString scoreSubtitle(const TestResult &r, const QStringList &parts);
// A metric's formatted value, or a dash.
QString metricText(const TestResult &r, const QString &key);
// Whether the option list `key` (a MultiSelect) contains `item`; true if the
// option is absent (defaults apply).
bool optionContains(const OptionValues &o, const QString &key, const QString &item);

} // namespace sb::tests
