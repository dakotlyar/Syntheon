// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/database.h"

#include <QLockFile>
#include <QSqlDatabase>
#include <QString>

#include <memory>

namespace sb {

class SettingsStore;

// Owns the history location and the UI thread's connection to synbench.db.
//
// Data location (P24): QStandardPaths::GenericDataLocation + "/SynBench"
// (%LOCALAPPDATA%\SynBench, ~/.local/share/SynBench) unless settings.ini says
// otherwise. Changing it moves the history, never merges it (D7). It can't be
// changed, and history can't be cleared, while a run is active or a recovery
// is pending (setBusy).
class Storage
{
public:
    enum class OpenStatus {
        Ok,
        NewerVersion,         // made by a newer SynBench: Choose another location / Quit
        LocationUnavailable,  // missing or unwritable (removed USB drive): default / Choose… / Quit
        Failed,
        Damaged,              // not a readable SynBench history; the file was left untouched
        InUse,                // another SynBench has this history open (single instance)
    };
    struct OpenResult {
        OpenStatus status = OpenStatus::Failed;
        QString error;
        QString backupPath;  // set when a migration backed the file up
        bool ok() const { return status == OpenStatus::Ok; }
    };

    enum class TargetState { NoDatabase, HasDatabase, Unusable };

    enum class MoveStatus {
        Moved,
        TargetHasDatabase,  // nothing changed: ask "Open the history found there?"
        Refused,            // a run is active or a recovery is pending
        Failed,             // nothing changed: the old location is still in use
    };
    struct MoveResult {
        MoveStatus status = MoveStatus::Failed;
        QString error;
        QString warning;  // moved, but e.g. the old file couldn't be deleted
    };

    explicit Storage(SettingsStore &settings);
    ~Storage();
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    static QString defaultDataDir();

    // The folder in use (after open()), or the configured/default one before.
    QString dataDir() const;
    QString dbPath() const;

    // Opens the configured location (the default one is created if missing),
    // migrating the database as needed.
    OpenResult open();
    // "Take over" after an in-use answer nobody could confirm: deletes the
    // lock file for the configured folder, then opens.
    OpenResult takeOverLock();
    // Launch dialog option "Use default location": forgets the custom folder.
    OpenResult useDefaultLocation();
    // Opens (creating if needed) the history in `folder` and makes it the data
    // location. The current history stays where it is. Used for "Open the
    // history found there?" (D7) and for "Choose…" at launch. On failure
    // nothing changes.
    OpenResult switchTo(const QString &folder);

    bool isOpen() const;
    // The UI thread's connection. Other threads open their own via
    // Database::openConnection(dbPath(), …).
    QSqlDatabase connection() const;

    void setBusy(bool busy) { m_busy = busy; }
    bool isBusy() const { return m_busy; }

    bool clearHistory(QString *error = nullptr);

    TargetState inspectTarget(const QString &folder, QString *error = nullptr) const;
    // D7: checkpoint + close, copy synbench.db to `folder`, verify the copy
    // (integrity_check + run count), save the setting, delete the old file.
    MoveResult moveTo(const QString &folder);

private:
    OpenResult openAt(const QString &folder, bool createFolder);
    void closeConnection();
    static bool isWritableDir(const QString &folder, QString *error);
    static OpenResult fromMigrate(const Database::MigrateResult &m);

    SettingsStore &m_settings;
    QString m_dataDir;
    QString m_connectionName;
    // One SynBench per history folder: held for as long as the folder is in use.
    std::unique_ptr<QLockFile> m_lock;
    QString m_lockDir;
    bool m_busy = false;
};

} // namespace sb
