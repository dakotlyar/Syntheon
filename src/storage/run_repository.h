#pragma once

#include "core/log_line.h"
#include "core/result.h"

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QUuid>
#include <QVector>

#include <optional>

namespace sb {

// Reads and writes runs on one connection (the caller's thread's). A run is
// written as it goes (P9): insertRun() with status 'running', one
// insertTestResult() per finished test (its own transaction), then
// finishRun(). load() returns the full RunRecord.
class RunRepository
{
public:
    explicit RunRepository(QSqlDatabase db) : m_db(std::move(db)) {}

    // Inserts the runs row (record.tests are ignored). Returns its id.
    std::optional<qint64> insertRun(const RunRecord &record);
    // One transaction: the test_results row plus its sub-results, metrics and series.
    bool insertTestResult(qint64 runId, int position, const TestResult &result);
    bool finishRun(qint64 runId, RunStatus status, const QDateTime &finishedAt, std::optional<double> overall);
    // insertRun + every test + finishRun, in one transaction (fixtures, imports).
    std::optional<qint64> saveRun(const RunRecord &record);

    std::optional<qint64> runId(const QUuid &uuid);
    std::optional<RunRecord> load(const QUuid &uuid);
    int runCount();

    bool appendLog(qint64 runId, const QVector<LogLine> &lines);
    QVector<LogLine> loadLog(qint64 runId);

    bool addCheckpoint(qint64 runId, const QString &testId, int seq, const QByteArray &payload);
    bool addScratchPath(qint64 runId, const QString &path);
    bool removeScratchPath(qint64 runId, const QString &path);

    // Clear history: DELETE FROM runs (cascades to everything), then VACUUM.
    bool clearHistory();

    QString lastError() const { return m_error; }

    // UTC ISO-8601 with milliseconds, the stored time format.
    static QString formatTime(const QDateTime &t);
    static QDateTime parseTime(const QString &s);
    // Series points as packed little-endian float64 (x, y) pairs.
    static QByteArray packPoints(const QVector<QPointF> &points);
    static QVector<QPointF> unpackPoints(const QByteArray &blob);

private:
    bool insertTestResultRows(qint64 runId, int position, const TestResult &result);
    bool fail(const QString &context, const QString &error);

    QSqlDatabase m_db;
    QString m_error;
};

} // namespace sb
