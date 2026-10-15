// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/log_line.h"
#include "core/result.h"

#include <QByteArray>
#include <QString>
#include <QUuid>
#include <QVector>

namespace sb {

// Where the orchestrator writes a run as it goes (§3 "Persistence while
// running"). core can't depend on storage (storage depends on core), so the
// orchestrator talks to this interface and synbench_storage implements it on
// SQLite (SqliteRunSink).
//
// Calls normally come from the run worker thread. The one exception: when a
// test doesn't respond after the grace period, the orchestrator finalises the
// run from its own (UI) thread. Calls are never concurrent; implementations
// keep one connection per calling thread and close it in threadFinished().
class RunSink
{
public:
    virtual ~RunSink() = default;

    // Inserts the run row with status 'running' plus one placeholder row per
    // planned test (run.tests: NotRun, "Queued"), in one transaction. The
    // placeholders record the plan, so recovery after a crash knows which
    // tests never finished (§3).
    virtual bool beginRun(const RunRecord &run, QString *error) = 0;
    // One finished test, committed in its own transaction; replaces that
    // test's placeholder row.
    virtual bool commitTest(const QUuid &run, int position, const TestResult &result, QString *error) = 0;
    virtual bool appendLog(const QUuid &run, const QVector<LogLine> &lines, QString *error) = 0;
    // Writes status, finished_at and the overall score.
    virtual bool finishRun(const RunRecord &run, QString *error) = 0;

    virtual bool addCheckpoint(const QUuid &run, const QString &testId, int seq, const QByteArray &cbor,
                               QString *error) = 0;
    virtual bool addScratchPath(const QUuid &run, const QString &path, QString *error) = 0;
    virtual bool removeScratchPath(const QUuid &run, const QString &path, QString *error) = 0;

    // §3 checkpoint durability: synchronous = FULL while a recoverable test runs.
    virtual void setDurable(bool /*full*/) {}
    // The calling thread is done with the sink (close its connection).
    virtual void threadFinished() {}
};

} // namespace sb
