// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/controllers/storage_controller.h"

#include "storage/run_repository.h"
#include "storage/settings_store.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QUrl>

namespace sb {

StorageController::StorageController(Storage &storage, SettingsStore &settings, QObject *parent)
    : QObject(parent), m_storage(storage), m_settings(settings)
{
}

void StorageController::open()
{
    const Storage::OpenResult r = m_storage.open();
    switch (r.status) {
    case Storage::OpenStatus::Ok:
        m_problem = StartupProblem::None;
        m_message.clear();
        break;
    case Storage::OpenStatus::LocationUnavailable:
        m_problem = StartupProblem::LocationUnavailable;
        m_message = QStringLiteral("The folder for your saved runs isn't available (for example a drive that was "
                                   "removed).\n%1").arg(r.error);
        break;
    case Storage::OpenStatus::NewerVersion:
        m_problem = StartupProblem::NewerVersion;
        m_message = QStringLiteral("The saved runs in %1 were made by a newer SynBench. This version can't open them "
                                   "and won't change them.").arg(m_storage.dataDir());
        break;
    case Storage::OpenStatus::InUse:
        m_problem = StartupProblem::InUse;
        m_message = QStringLiteral("SynBench is already running with this history.");
        break;
    case Storage::OpenStatus::Damaged:
        m_problem = StartupProblem::Failed;
        m_message = QStringLiteral("The history file at %1 is damaged or isn't a SynBench history (%2). "
                                   "It was left untouched: use another location, or move the file away and start again.")
                        .arg(QDir::toNativeSeparators(m_storage.dbPath()), r.error.isEmpty() ? QStringLiteral("unknown error") : r.error);
        break;
    case Storage::OpenStatus::Failed:
        m_problem = StartupProblem::Failed;
        m_message = QStringLiteral("The history at %1 couldn't be opened (%2).")
                        .arg(QDir::toNativeSeparators(m_storage.dbPath()), r.error.isEmpty() ? QStringLiteral("unknown error") : r.error);
        break;
    }
    emit startupProblemChanged();
    emit locationChanged();
    emit historyChanged();
}

QString StorageController::startupKind() const
{
    switch (m_problem) {
    case StartupProblem::None: return QStringLiteral("none");
    case StartupProblem::LocationUnavailable: return QStringLiteral("location");
    case StartupProblem::NewerVersion: return QStringLiteral("newer");
    case StartupProblem::Failed: return QStringLiteral("failed");
    case StartupProblem::InUse: return QStringLiteral("inuse");
    }
    return {};
}

QString StorageController::apply(const Storage::OpenResult &r)
{
    if (r.ok()) {
        m_problem = StartupProblem::None;
        m_message.clear();
        emit startupProblemChanged();
        emit locationChanged();
        emit historyChanged();
        return {};
    }
    if (r.status == Storage::OpenStatus::NewerVersion)
        return QStringLiteral("The saved runs there were made by a newer SynBench.");
    if (r.status == Storage::OpenStatus::Damaged)
        return QStringLiteral("The history file there is damaged or isn't a SynBench history (%1). It was left untouched.").arg(r.error);
    if (r.status == Storage::OpenStatus::InUse)
        return QStringLiteral("That history is open in another SynBench. Close it there first.");
    return r.error.isEmpty() ? QStringLiteral("That folder can't be used.") : r.error;
}

QString StorageController::dataLocation() const
{
    return QDir::toNativeSeparators(m_storage.dataDir());
}

QString StorageController::dbPath() const
{
    return QDir::toNativeSeparators(m_storage.dbPath());
}

int StorageController::runCount() const
{
    if (!m_storage.isOpen())
        return 0;
    return std::max(0, RunRepository(m_storage.connection()).runCount());
}

QString StorageController::sizeText() const
{
    qint64 bytes = 0;
    for (const char *suffix : {"", "-wal"})
        bytes += QFileInfo(m_storage.dbPath() + QLatin1StringView(suffix)).size();
    return QLocale(QLocale::English).formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString StorageController::lockedReason() const
{
    // The run's reason first: it's the one the user can act on.
    if (m_locks.contains(QStringLiteral("run")))
        return m_locks.value(QStringLiteral("run"));
    return m_locks.isEmpty() ? QString() : m_locks.constBegin().value();
}

void StorageController::lock(const QString &key, const QString &reason)
{
    m_locks.insert(key, reason);
    m_storage.setBusy(true);
    emit lockedChanged();
}

void StorageController::unlock(const QString &key)
{
    if (m_locks.remove(key) == 0)
        return;
    m_storage.setBusy(!m_locks.isEmpty());
    emit lockedChanged();
}

void StorageController::setLocked(bool locked, const QString &reason)
{
    if (locked)
        lock(QStringLiteral("run"), reason);
    else
        unlock(QStringLiteral("run"));
}

QString StorageController::takeOver()
{
    const Storage::OpenResult r = m_storage.takeOverLock();
    return apply(r);
}

QString StorageController::urlToPath(const QUrl &url) const
{
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}

QString StorageController::inspectFolder(const QString &folder)
{
    QString error;
    switch (m_storage.inspectTarget(folder, &error)) {
    case Storage::TargetState::HasDatabase:
        return QStringLiteral("database");
    case Storage::TargetState::NoDatabase:
        return QStringLiteral("empty");
    case Storage::TargetState::Unusable:
        return QStringLiteral("unusable");
    }
    return QStringLiteral("unusable");
}

QString StorageController::moveHistoryTo(const QString &folder)
{
    const Storage::MoveResult r = m_storage.moveTo(folder);
    switch (r.status) {
    case Storage::MoveStatus::Moved:
        emit locationChanged();
        emit historyChanged();
        // Moved. A leftover old file is a warning, not a failure.
        return r.warning.isEmpty() ? QString() : QStringLiteral("warning:") + r.warning;
    case Storage::MoveStatus::TargetHasDatabase:
        return QStringLiteral("That folder already has saved runs.");
    case Storage::MoveStatus::Refused:
    case Storage::MoveStatus::Failed:
        return r.error.isEmpty() ? QStringLiteral("The history couldn't be moved.") : r.error;
    }
    return {};
}

QString StorageController::openHistoryIn(const QString &folder)
{
    return apply(m_storage.switchTo(folder));
}

QString StorageController::useDefaultLocation()
{
    return apply(m_storage.useDefaultLocation());
}

QString StorageController::clearHistory()
{
    QString error;
    if (!m_storage.clearHistory(&error))
        return error;
    emit historyChanged();
    return {};
}

} // namespace sb
