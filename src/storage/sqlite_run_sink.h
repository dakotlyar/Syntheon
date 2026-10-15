// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/run_sink.h"

#include <QHash>
#include <QMutex>
#include <QSqlDatabase>
#include <QString>

namespace sb {

class RunRepository;

// RunSink on synbench.db: one connection per calling thread (normally the run
// worker), opened on first use and closed by threadFinished(). Uses
// synchronous = NORMAL, switched to FULL by setDurable(true) while a
// recoverable test runs (§3).
class SqliteRunSink : public RunSink
{
public:
    explicit SqliteRunSink(QString dbPath);
    ~SqliteRunSink() override;

    bool beginRun(const RunRecord &run, QString *error) override;
    bool commitTest(const QUuid &run, int position, const TestResult &result, QString *error) override;
    bool appendLog(const QUuid &run, const QVector<LogLine> &lines, QString *error) override;
    bool finishRun(const RunRecord &run, QString *error) override;
    bool addCheckpoint(const QUuid &run, const QString &testId, int seq, const QByteArray &cbor,
                       QString *error) override;
    bool addScratchPath(const QUuid &run, const QString &path, QString *error) override;
    bool removeScratchPath(const QUuid &run, const QString &path, QString *error) override;
    void setDurable(bool full) override;
    void threadFinished() override;

private:
    QSqlDatabase connection(QString *error);
    std::optional<qint64> runId(RunRepository &repo, const QUuid &run, QString *error);

    const QString m_path;
    QMutex m_mutex;
    QHash<Qt::HANDLE, QString> m_connections;  // thread -> connection name
    QHash<QUuid, qint64> m_runIds;
};

} // namespace sb
