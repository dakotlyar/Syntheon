// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/sqlite_run_sink.h"

#include "storage/database.h"
#include "storage/run_repository.h"

#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

namespace sb {

SqliteRunSink::SqliteRunSink(PathProvider dbPath) : m_path(std::move(dbPath)) {}

SqliteRunSink::SqliteRunSink(const QString &dbPath) : m_path([dbPath] { return dbPath; }) {}

SqliteRunSink::~SqliteRunSink()
{
    // Connections are per thread and should have been closed by
    // threadFinished(); close any left over from this thread.
    threadFinished();
}

QSqlDatabase SqliteRunSink::connection(QString *error)
{
    const Qt::HANDLE thread = QThread::currentThreadId();
    const QString path = m_path();
    Connection existing;
    {
        QMutexLocker lock(&m_mutex);
        existing = m_connections.value(thread);
    }
    if (!existing.name.isEmpty()) {
        if (existing.path == path)
            return QSqlDatabase::database(existing.name, false);
        threadFinished();  // the data location moved since this thread connected
    }

    // Never create a database here: a missing file means the location moved
    // or vanished, and SQLite would silently make an empty one.
    if (!QFileInfo::exists(path)) {
        if (error)
            *error = QStringLiteral("The history database %1 doesn't exist").arg(path);
        return {};
    }
    const QString name = Database::uniqueConnectionName(QStringLiteral("run"));
    QSqlDatabase db = Database::openConnection(path, name, error);
    if (!db.isOpen()) {
        db = QSqlDatabase();
        Database::closeConnection(name);
        return {};
    }
    QSqlQuery(db).exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    QMutexLocker lock(&m_mutex);
    m_connections.insert(thread, {name, path});
    if (m_runIdsPath != path) {
        m_runIds.clear();  // ids belong to the database they came from
        m_runIdsPath = path;
    }
    return db;
}

void SqliteRunSink::threadFinished()
{
    Connection c;
    {
        QMutexLocker lock(&m_mutex);
        c = m_connections.take(QThread::currentThreadId());
    }
    if (!c.name.isEmpty())
        Database::closeConnection(c.name);
}

std::optional<qint64> SqliteRunSink::runId(RunRepository &repo, const QUuid &run, QString *error)
{
    {
        QMutexLocker lock(&m_mutex);
        const auto it = m_runIds.constFind(run);
        if (it != m_runIds.cend())
            return *it;
    }
    const auto id = repo.runId(run);
    if (!id) {
        if (error)
            *error = repo.lastError().isEmpty() ? QStringLiteral("unknown run %1").arg(run.toString())
                                                : repo.lastError();
        return std::nullopt;
    }
    QMutexLocker lock(&m_mutex);
    m_runIds.insert(run, *id);
    return id;
}

#define SB_SINK_REPO(error)               \
    QSqlDatabase db = connection(error);  \
    if (!db.isOpen())                     \
        return false;                     \
    RunRepository repo(db)

bool SqliteRunSink::beginRun(const RunRecord &run, QString *error)
{
    SB_SINK_REPO(error);
    const auto id = repo.saveRun(run);  // run row + placeholders, one transaction
    if (!id) {
        if (error)
            *error = repo.lastError();
        return false;
    }
    QMutexLocker lock(&m_mutex);
    m_runIds.insert(run.uuid, *id);
    return true;
}

bool SqliteRunSink::commitTest(const QUuid &run, int position, const TestResult &result, QString *error)
{
    SB_SINK_REPO(error);
    const auto id = runId(repo, run, error);
    if (!id)
        return false;
    if (repo.insertTestResult(*id, position, result))
        return true;
    if (error)
        *error = repo.lastError();
    return false;
}

bool SqliteRunSink::appendLog(const QUuid &run, const QVector<LogLine> &lines, QString *error)
{
    SB_SINK_REPO(error);
    const auto id = runId(repo, run, error);
    if (!id)
        return false;
    if (repo.appendLog(*id, lines))
        return true;
    if (error)
        *error = repo.lastError();
    return false;
}

bool SqliteRunSink::finishRun(const RunRecord &run, QString *error)
{
    SB_SINK_REPO(error);
    const auto id = runId(repo, run.uuid, error);
    if (!id)
        return false;
    if (repo.finishRun(*id, run.status, run.finishedAt, run.overall)) {
        QMutexLocker lock(&m_mutex);
        m_runIds.remove(run.uuid);  // the run is final; nothing more is written to it
        return true;
    }
    if (error)
        *error = repo.lastError();
    return false;
}

bool SqliteRunSink::addCheckpoint(const QUuid &run, const QString &testId, int seq, const QByteArray &cbor,
                                  QString *error)
{
    SB_SINK_REPO(error);
    const auto id = runId(repo, run, error);
    if (!id)
        return false;
    if (repo.addCheckpoint(*id, testId, seq, cbor))
        return true;
    if (error)
        *error = repo.lastError();
    return false;
}

bool SqliteRunSink::addScratchPath(const QUuid &run, const QString &path, QString *error)
{
    SB_SINK_REPO(error);
    const auto id = runId(repo, run, error);
    if (!id)
        return false;
    if (repo.addScratchPath(*id, path))
        return true;
    if (error)
        *error = repo.lastError();
    return false;
}

bool SqliteRunSink::removeScratchPath(const QUuid &run, const QString &path, QString *error)
{
    SB_SINK_REPO(error);
    const auto id = runId(repo, run, error);
    if (!id)
        return false;
    if (repo.removeScratchPath(*id, path))
        return true;
    if (error)
        *error = repo.lastError();
    return false;
}

#undef SB_SINK_REPO

void SqliteRunSink::setDurable(bool full)
{
    QSqlDatabase db = connection(nullptr);
    if (db.isOpen())
        QSqlQuery(db).exec(full ? QStringLiteral("PRAGMA synchronous = FULL") : QStringLiteral("PRAGMA synchronous = NORMAL"));
}

} // namespace sb
