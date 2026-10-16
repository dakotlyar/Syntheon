// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/controllers/storage_controller.h"

#include "storage/recovery.h"

#include <QObject>
#include <QSet>
#include <QThread>
#include <QVariantList>

#include <memory>

namespace sb {

class RunController;
class SettingsController;
class StorageController;
class Registry;

// QML singleton "Startup": the two things that happen once, before the app
// is used. (1) Recovery (design 3): runs still marked 'running' from an
// earlier session are finished off (recovered or interrupted), their scratch
// folders deleted, and a one-time notice is shown. The history is locked
// while that runs. (2) The first-run dialog: a hardware check from the tests'
// probes and where the history is kept.
class StartupController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString instanceWarning READ instanceWarning NOTIFY instanceWarningChanged)
    // Recovery is still running after a second: worth a quiet "Checking earlier runs…".
    Q_PROPERTY(bool recoverySlow READ recoverySlow NOTIFY recoveringChanged)
    Q_PROPERTY(bool recovering READ recovering NOTIFY recoveringChanged)
    Q_PROPERTY(QString recoveryNotice READ recoveryNotice NOTIFY noticeChanged)
    Q_PROPERTY(bool recoveryPending READ recoveryPending NOTIFY noticeChanged)
    Q_PROPERTY(bool firstRunNeeded READ firstRunNeeded NOTIFY firstRunChanged)
    Q_PROPERTY(QVariantList hardwareCheck READ hardwareCheck NOTIFY hardwareChanged)
    Q_PROPERTY(bool hardwareReady READ hardwareReady NOTIFY hardwareChanged)

public:
    StartupController(RunController &run, StorageController &storage, SettingsController &settings, QObject *parent = nullptr);

    ~StartupController() override;

    // Runs recovery on the open history, off the GUI thread (own database
    // connection), with the history locked and `recovering` true meanwhile.
    // Skipped without an open history or while a run is live; a call while
    // one is in flight just asks for another pass afterwards. Also runs when
    // the location changes. A pending notice is never replaced by "nothing
    // to report": it stays until the user dismisses it.
    void recover();
    bool recovering() const { return m_thread != nullptr; }
    bool recoverySlow() const { return m_thread != nullptr && m_slow; }
    QString instanceWarning() const { return m_instanceWarning; }
    void setInstanceWarning(const QString &w);
    // Applies a finished pass (the worker thread queues this; tests call it).
    void finishRecovery(const RecoveryReport &report);
    // Blocks until recovery has finished (tests, shutdown).
    void waitForRecovery();

    QString recoveryNotice() const { return m_notice; }
    bool recoveryPending() const { return !m_notice.isEmpty(); }
    bool firstRunNeeded() const;
    QVariantList hardwareCheck() const;
    bool hardwareReady() const;

    Q_INVOKABLE void dismissRecoveryNotice();
    Q_INVOKABLE void finishFirstRun();

    static QString hoursMinutes(qint64 ms);
    static constexpr int kSlowRecoveryMs = 1000;

signals:
    void recoveringChanged();
    void instanceWarningChanged();
    void recoveryFinished();
    void noticeChanged();
    void firstRunChanged();
    void hardwareChanged();

private:
    RunController &m_run;
    StorageController &m_storage;
    SettingsController &m_settings;
    QString m_notice;
    QString m_instanceWarning;
    bool m_slow = false;
    QSet<QString> m_seenProblems;
    QThread *m_thread = nullptr;
    bool m_again = false;
    std::unique_ptr<StorageController::ScopedLock> m_lock;
};

} // namespace sb
