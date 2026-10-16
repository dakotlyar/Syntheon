// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QVariantList>

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
    Q_PROPERTY(QString recoveryNotice READ recoveryNotice NOTIFY noticeChanged)
    Q_PROPERTY(bool recoveryPending READ recoveryPending NOTIFY noticeChanged)
    Q_PROPERTY(bool firstRunNeeded READ firstRunNeeded NOTIFY firstRunChanged)
    Q_PROPERTY(QVariantList hardwareCheck READ hardwareCheck NOTIFY hardwareChanged)
    Q_PROPERTY(bool hardwareReady READ hardwareReady NOTIFY hardwareChanged)

public:
    StartupController(RunController &run, StorageController &storage, SettingsController &settings, QObject *parent = nullptr);

    // Runs recovery on the open history (a no-op without one, or while a run
    // of this session is active). Also runs when the location changes.
    void recover();

    QString recoveryNotice() const { return m_notice; }
    bool recoveryPending() const { return !m_notice.isEmpty(); }
    bool firstRunNeeded() const;
    QVariantList hardwareCheck() const;
    bool hardwareReady() const;

    Q_INVOKABLE void dismissRecoveryNotice();
    Q_INVOKABLE void finishFirstRun();

    static QString hoursMinutes(qint64 ms);

signals:
    void noticeChanged();
    void firstRunChanged();
    void hardwareChanged();

private:
    RunController &m_run;
    StorageController &m_storage;
    SettingsController &m_settings;
    QString m_notice;
};

} // namespace sb
