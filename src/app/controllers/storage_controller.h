// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/storage.h"

#include <QMap>
#include <QObject>
#include <QString>

namespace sb {

class SettingsStore;

// QML singleton "StorageController": the history location (Settings screen and
// the launch problems of §4), Clear history, and the numbers shown next to
// them. All results are plain strings (empty = success) so QML needs no
// C++ types.
class StorageController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(StartupProblem startupProblem READ startupProblem NOTIFY startupProblemChanged)
    Q_PROPERTY(QString startupMessage READ startupMessage NOTIFY startupProblemChanged)
    Q_PROPERTY(QString startupKind READ startupKind NOTIFY startupProblemChanged)
    Q_PROPERTY(QString dataLocation READ dataLocation NOTIFY locationChanged)
    Q_PROPERTY(QString dbPath READ dbPath NOTIFY locationChanged)
    Q_PROPERTY(int runCount READ runCount NOTIFY historyChanged)
    Q_PROPERTY(QString sizeText READ sizeText NOTIFY historyChanged)
    Q_PROPERTY(bool locked READ locked NOTIFY lockedChanged)
    Q_PROPERTY(QString lockedReason READ lockedReason NOTIFY lockedChanged)

public:
    enum class StartupProblem { None, LocationUnavailable, NewerVersion, Failed, InUse };
    Q_ENUM(StartupProblem)

    StorageController(Storage &storage, SettingsStore &settings, QObject *parent = nullptr);

    // Opens the configured location. On a problem, startupProblem is set and
    // QML offers Use default location / Choose… / Quit (§4).
    void open();
    StartupProblem startupProblem() const { return m_problem; }
    QString startupMessage() const { return m_message; }
    // "none" | "location" | "newer" | "failed" (QML compares strings).
    QString startupKind() const;

    QString dataLocation() const;
    QString dbPath() const;
    int runCount() const;
    QString sizeText() const;
    bool locked() const { return !m_locks.isEmpty(); }
    bool lockedBy(const QString &key) const { return m_locks.contains(key); }
    QString lockedReason() const;

    // A run or a pending recovery pins the location and Clear history (§3).
    // Locks are keyed ("run", "recovery"): each key is held or not, so two
    // holders can overlap and releasing one never frees the other.
    void lock(const QString &key, const QString &reason);
    void unlock(const QString &key);
    // The run's lock (kept for the run controller).
    void setLocked(bool locked, const QString &reason = {});

    // Holds a lock for a scope, released on every exit path.
    class ScopedLock
    {
    public:
        ScopedLock(StorageController &c, const QString &key, const QString &reason) : m_c(&c), m_key(key) { c.lock(key, reason); }
        ~ScopedLock() { release(); }
        void release()
        {
            if (m_c)
                m_c->unlock(m_key);
            m_c = nullptr;
        }
        ScopedLock(const ScopedLock &) = delete;
        ScopedLock &operator=(const ScopedLock &) = delete;

    private:
        StorageController *m_c;
        QString m_key;
    };

    Storage &storage() { return m_storage; }

    // "database" if the folder already holds a synbench.db (offer to open it,
    // D7), "empty" if the history can be moved there, else "unusable".
    Q_INVOKABLE QString inspectFolder(const QString &folder);
    // Move the history to `folder` (D7): "" on success, "warning:<text>" if it
    // moved but something was left behind, else why not.
    Q_INVOKABLE QString moveHistoryTo(const QString &folder);
    // Switch to the history already in `folder`, leaving the current one.
    Q_INVOKABLE QString openHistoryIn(const QString &folder);
    Q_INVOKABLE QString useDefaultLocation();
    Q_INVOKABLE QString clearHistory();
    // file:// URL from a dialog -> path.
    Q_INVOKABLE QString urlToPath(const QUrl &url) const;

signals:
    void startupProblemChanged();
    void locationChanged();
    void historyChanged();
    void lockedChanged();

private:
    QString apply(const Storage::OpenResult &r);

    Storage &m_storage;
    SettingsStore &m_settings;
    StartupProblem m_problem = StartupProblem::None;
    QString m_message;
    QMap<QString, QString> m_locks;  // key -> reason, in key order
};

} // namespace sb
