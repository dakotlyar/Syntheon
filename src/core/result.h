#pragma once

#include "core/enums.h"

#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QPointF>
#include <QString>
#include <QUuid>
#include <QVariantMap>
#include <QVector>

#include <functional>
#include <optional>
#include <variant>

namespace sb {

// One raw measurement or a text attribute. Keys are stable forever (they
// become JSON and PTS keys in Phase 9): never renamed, only deprecated.
struct Metric {
    QString key;                          // "seq1m_q8t1.read"
    QString subTestId;                    // optional
    QString label;                        // stored, so old runs still render
    std::variant<double, QString> value;  // 7012.0 or "AVX2"
    QString unit;                         // canonical unit, see Units
    Direction direction = Direction::Neutral;
    bool headline = false;                // shown on the Results row + Compare

    bool isNumeric() const { return std::holds_alternative<double>(value); }
    bool operator==(const Metric &) const = default;
};

// A curve: RAM latency vs block size, Battery drain over time.
struct Series {
    QString key, label, xLabel, xUnit, yUnit;
    QVector<QPointF> points;

    bool operator==(const Series &) const = default;
};

struct SubResult {
    QString id;
    Status status = Status::Ok;
    QString reason;
    std::optional<double> score;

    bool operator==(const SubResult &) const = default;
};

struct TestResult {
    QString testId;
    int testVersion = 0;
    int formulaVersion = 0;
    Status status = Status::Ok;
    QString statusReason;
    QVector<SubResult> subResults;
    QVector<Metric> metrics;
    QVector<Series> series;
    std::optional<double> score;  // filled in by the orchestrator from ITest::score()
    QVariantMap options;
    QDateTime startedAt;
    qint64 durationMs = 0;

    const Metric *metric(const QString &key) const;

    static TestResult withStatus(Status status, const QString &reason = {});
    static TestResult interrupted(const QString &reason = {}) { return withStatus(Status::Interrupted, reason); }
    static TestResult notAvailable(const QString &reason) { return withStatus(Status::NotAvailable, reason); }
    static TestResult error(const QString &reason) { return withStatus(Status::Error, reason); }

    bool operator==(const TestResult &) const = default;
};

// Output of the pure, versioned ITest::score() (P8). Computed once after the
// run and stored frozen; never recomputed for stored runs.
struct ScoreBreakdown {
    std::optional<double> total;
    QMap<QString, double> subScores;  // sub-test id -> score

    bool operator==(const ScoreBreakdown &) const = default;
};

// The machine the run was taken on. The collected fields live in `data` and
// grow per phase (GPU APIs, AI runtimes, NPU drivers), so the shape is JSON,
// versioned by `version` (sysinfo_version). machineLabel is copied out for the
// History list.
struct SystemSnapshot {
    static constexpr int kCurrentVersion = 1;

    int version = kCurrentVersion;
    QString machineLabel;
    QJsonObject data;

    QJsonObject toJson() const;
    static SystemSnapshot fromJson(const QJsonObject &json);

    bool operator==(const SystemSnapshot &) const = default;
};

// The one model shared by GUI, storage and (Phase 9) CLI/JSON.
struct RunRecord {
    QUuid uuid;
    RunKind kind = RunKind::Standard;
    RunStatus status = RunStatus::Running;
    QDateTime startedAt, finishedAt;
    QString appVersion, appBuild;  // "2026.11.0", "linux-x64 abc1234"
    SystemSnapshot system;
    QVector<TestResult> tests;
    std::optional<double> overall;  // sum of scores where countsTowardOverall && score present

    bool operator==(const RunRecord &) const = default;
};

// Overall = the sum of the available scores of tests that count toward it.
// Tests without a score contribute nothing. Returns nullopt when no counted
// test has a score.
std::optional<double> computeOverall(const QVector<TestResult> &tests,
                                     const std::function<bool(const QString &testId)> &countsTowardOverall);

} // namespace sb
