// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/storage.h"

#include "storage/run_repository.h"
#include "storage/settings_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace sb {

namespace {

QString normalized(const QString &folder)
{
    return QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
}

QString dbFileIn(const QString &folder)
{
    return QDir(folder).filePath(QString::fromLatin1(Database::kFileName));
}

bool removeDbFiles(const QString &dbPath)
{
    bool ok = !QFile::exists(dbPath) || QFile::remove(dbPath);
    for (const char *suffix : {"-wal", "-shm"}) {
        const QString f = dbPath + QLatin1StringView(suffix);
        if (QFile::exists(f))
            ok = QFile::remove(f) && ok;
    }
    return ok;
}

} // namespace

Storage::Storage(SettingsStore &settings) : m_settings(settings) {}

Storage::~Storage()
{
    closeConnection();
}

QString Storage::defaultDataDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("SynBench"));
}

QString Storage::dataDir() const
{
    if (!m_dataDir.isEmpty())
        return m_dataDir;
    const QString configured = m_settings.dataLocation();
    return configured.isEmpty() ? defaultDataDir() : normalized(configured);
}

QString Storage::dbPath() const
{
    return dbFileIn(dataDir());
}

bool Storage::isOpen() const
{
    return !m_connectionName.isEmpty() && QSqlDatabase::database(m_connectionName, false).isOpen();
}

QSqlDatabase Storage::connection() const
{
    return QSqlDatabase::database(m_connectionName, false);
}

void Storage::closeConnection()
{
    if (m_connectionName.isEmpty())
        return;
    Database::closeConnection(m_connectionName);
    m_connectionName.clear();
}

bool Storage::isWritableDir(const QString &folder, QString *error)
{
    const QFileInfo info(folder);
    if (!info.exists() || !info.isDir()) {
        if (error)
            *error = QStringLiteral("The folder %1 doesn't exist.").arg(folder);
        return false;
    }
    // A real write probe: QFileInfo::isWritable() isn't reliable on Windows ACLs.
    QTemporaryFile probe(QDir(folder).filePath(QStringLiteral(".synbench-write-test-XXXXXX")));
    if (!probe.open()) {
        if (error)
            *error = QStringLiteral("The folder %1 isn't writable.").arg(folder);
        return false;
    }
    return true;
}

Storage::OpenResult Storage::fromMigrate(const Database::MigrateResult &m)
{
    OpenResult r;
    r.error = m.error;
    r.backupPath = m.backupPath;
    switch (m.status) {
    case Database::MigrateResult::Status::Ok:
        r.status = OpenStatus::Ok;
        break;
    case Database::MigrateResult::Status::NewerVersion:
        r.status = OpenStatus::NewerVersion;
        break;
    case Database::MigrateResult::Status::Failed:
        r.status = OpenStatus::Failed;
        break;
    }
    return r;
}

Storage::OpenResult Storage::openAt(const QString &folder, bool createFolder)
{
    OpenResult r;
    if (createFolder && !QDir().mkpath(folder)) {
        r.status = OpenStatus::LocationUnavailable;
        r.error = QStringLiteral("Couldn't create the folder %1.").arg(folder);
        return r;
    }
    if (!isWritableDir(folder, &r.error)) {
        r.status = OpenStatus::LocationUnavailable;
        return r;
    }

    const QString path = dbFileIn(folder);
    r = fromMigrate(Database::migrate(path));
    if (!r.ok())
        return r;

    const QString name = Database::uniqueConnectionName(QStringLiteral("ui"));
    QString error;
    QSqlDatabase db = Database::openConnection(path, name, &error);
    if (!db.isOpen()) {
        db = QSqlDatabase();
        Database::closeConnection(name);
        r.status = OpenStatus::Failed;
        r.error = error;
        return r;
    }

    closeConnection();
    m_connectionName = name;
    m_dataDir = folder;
    return r;
}

Storage::OpenResult Storage::open()
{
    const QString configured = m_settings.dataLocation();
    if (configured.isEmpty())
        return openAt(defaultDataDir(), true);
    return openAt(normalized(configured), false);
}

Storage::OpenResult Storage::useDefaultLocation()
{
    OpenResult r = openAt(defaultDataDir(), true);
    if (r.ok()) {
        m_settings.setDataLocation({});
        m_settings.sync();
    }
    return r;
}

Storage::OpenResult Storage::switchTo(const QString &folder)
{
    if (m_busy)
        return {OpenStatus::Failed, QStringLiteral("The data location can't change while a run is active.")};
    const QString target = normalized(folder);
    const QString previous = m_dataDir;
    OpenResult r = openAt(target, false);
    if (!r.ok())
        return r;
    m_settings.setDataLocation(target);
    if (!m_settings.sync()) {
        r = {OpenStatus::Failed, QStringLiteral("Couldn't save the setting to %1.").arg(m_settings.fileName())};
        if (!previous.isEmpty())
            openAt(previous, false);
    }
    return r;
}

bool Storage::clearHistory(QString *error)
{
    if (m_busy) {
        if (error)
            *error = QStringLiteral("History can't be cleared while a run is active.");
        return false;
    }
    if (!isOpen()) {
        if (error)
            *error = QStringLiteral("The history database isn't open.");
        return false;
    }
    RunRepository repo(connection());
    if (!repo.clearHistory()) {
        if (error)
            *error = repo.lastError();
        return false;
    }
    return true;
}

Storage::TargetState Storage::inspectTarget(const QString &folder, QString *error) const
{
    const QString target = normalized(folder);
    if (QFileInfo::exists(dbFileIn(target)))
        return TargetState::HasDatabase;
    return isWritableDir(target, error) ? TargetState::NoDatabase : TargetState::Unusable;
}

Storage::MoveResult Storage::moveTo(const QString &folder)
{
    MoveResult result;
    if (m_busy) {
        result.status = MoveStatus::Refused;
        result.error = QStringLiteral("The data location can't change while a run is active.");
        return result;
    }
    if (!isOpen()) {
        result.error = QStringLiteral("The history database isn't open.");
        return result;
    }

    const QString target = normalized(folder);
    const QString oldDir = m_dataDir;
    if (target == oldDir) {
        result.error = QStringLiteral("That is already the data location.");
        return result;
    }
    switch (inspectTarget(target, &result.error)) {
    case TargetState::HasDatabase:
        result.status = MoveStatus::TargetHasDatabase;
        return result;
    case TargetState::Unusable:
        return result;
    case TargetState::NoDatabase:
        break;
    }

    const QString oldPath = dbFileIn(oldDir);
    const QString newPath = dbFileIn(target);

    int runCount = -1;
    {
        RunRepository repo(connection());
        runCount = repo.runCount();
        if (runCount < 0) {
            result.error = repo.lastError();
            return result;
        }
    }

    // 1. Checkpoint and close, so synbench.db alone holds everything.
    closeConnection();
    const auto restore = [&](const QString &error) {
        result.status = MoveStatus::Failed;
        result.error = error;
        const OpenResult reopened = openAt(oldDir, false);
        if (!reopened.ok())
            result.error += QStringLiteral(" (and reopening the old history failed: %1)").arg(reopened.error);
        return result;
    };
    {
        Database::ScopedConnection conn(oldPath, QStringLiteral("move-checkpoint"));
        if (!conn.isOpen())
            return restore(conn.error());
        QSqlQuery q(conn.db());
        if (!q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")) || !q.next())
            return restore(q.lastError().text());
        if (q.value(0).toInt() != 0)
            return restore(QStringLiteral("The history database is busy; try again."));
    }

    // 2. Copy and verify the copy.
    if (!QFile::copy(oldPath, newPath))
        return restore(QStringLiteral("Couldn't copy the history to %1.").arg(target));
    QString verifyError;
    {
        Database::ScopedConnection conn(newPath, QStringLiteral("move-verify"));
        if (!conn.isOpen()) {
            verifyError = conn.error();
        } else {
            QSqlQuery q(conn.db());
            if (!q.exec(QStringLiteral("PRAGMA integrity_check")) || !q.next()
                || q.value(0).toString() != QLatin1StringView("ok")) {
                verifyError = QStringLiteral("The copied history failed its integrity check.");
            } else if (RunRepository(conn.db()).runCount() != runCount) {
                verifyError = QStringLiteral("The copied history doesn't have every run.");
            }
        }
    }
    if (!verifyError.isEmpty()) {
        removeDbFiles(newPath);
        return restore(verifyError);
    }

    // 3. Switch to the copy and save the setting.
    const OpenResult opened = openAt(target, false);
    if (!opened.ok()) {
        removeDbFiles(newPath);
        return restore(opened.error);
    }
    m_settings.setDataLocation(target);
    if (!m_settings.sync()) {
        closeConnection();
        removeDbFiles(newPath);
        m_settings.setDataLocation(oldDir == defaultDataDir() ? QString() : oldDir);
        return restore(QStringLiteral("Couldn't save the setting to %1.").arg(m_settings.fileName()));
    }

    // 4. Delete the old file.
    result.status = MoveStatus::Moved;
    if (!removeDbFiles(oldPath))
        result.warning = QStringLiteral("The history moved, but the old file %1 couldn't be deleted.").arg(oldPath);
    return result;
}

} // namespace sb
