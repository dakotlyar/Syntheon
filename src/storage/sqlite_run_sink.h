// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/run_sink.h"

#include <QHash>
#include <QMutex>
#include <QSqlDatabase>
#include <QString>

#include <functional>
#include <optional>

namespace sb {

class RunRepository;

// RunSink on synbench.db: one connection per calling thread (normally the run
// worker), opened on first use and closed by threadFinished(). Uses
// synchronous = NORMAL, switched to FULL by setDurable(true) while a
// recoverable test runs (§3).
//
// The database path comes from a provider (normally Storage::dbPath()), read
// whenever a connection is opened, so a run after a data-location move writes
// to the new file (F2). A missing file is never created here.
class SqliteRunSink : public RunSink
{
public:
    using PathProvider = std::function<QString()>;

    explicit SqliteRunSink(PathProvider dbPath);
    explicit SqliteRunSink(const QString &dbPath);
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

    struct Connection {
        QString name;
        QString path;
    };

    const PathProvider m_path;
    QMutex m_mutex;
    QHash<Qt::HANDLE, Connection> m_connections;  // per thread
    QHash<QUuid, qint64> m_runIds;                // valid for m_runIdsPath only
    QString m_runIdsPath;
};

} // namespace sb
