// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/controllers/startup_controller.h"

#include "app/controllers/run_controller.h"
#include "app/controllers/settings_controller.h"
#include "app/controllers/storage_controller.h"
#include "storage/database.h"
#include "storage/recovery.h"
#include "storage/storage.h"

#include <QCoreApplication>
#include <QLocale>
#include <QStringList>
#include <QTimer>

namespace sb {

StartupController::StartupController(RunController &run, StorageController &storage, SettingsController &settings, QObject *parent)
    : QObject(parent), m_run(run), m_storage(storage), m_settings(settings)
{
    connect(&m_storage, &StorageController::locationChanged, this, &StartupController::recover);
    connect(&m_settings, &SettingsController::firstRunDoneChanged, this, &StartupController::firstRunChanged);
    connect(&m_run, &RunController::probingChanged, this, &StartupController::hardwareChanged);
    connect(&m_run, &RunController::selectionChanged, this, &StartupController::hardwareChanged);
}

QString StartupController::hoursMinutes(qint64 ms)
{
    const qint64 m = (ms + 30000) / 60000;
    return QStringLiteral("%1 h %2 m").arg(m / 60).arg(m % 60, 2, 10, QLatin1Char('0'));
}

StartupController::~StartupController()
{
    waitForRecovery();
}

void StartupController::waitForRecovery()
{
    // The worker queues its result to this object, so keep delivering events.
    while (m_thread)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void StartupController::recover()
{
    if (m_thread) {
        m_again = true;
        return;
    }
    // Only a live run (or an abandoned one awaiting restart) has a 'running'
    // row of its own; a finished one from earlier in the session doesn't.
    if (!m_storage.storage().isOpen() || m_run.running() || m_run.needsRestart())
        return;
    m_again = false;
    m_lock = std::make_unique<StorageController::ScopedLock>(m_storage, QStringLiteral("recovery"),
                                                             tr("Checking earlier runs…"));
    const QString dbPath = m_storage.storage().dbPath();
    const Registry *registry = &m_run.registry();
    const QString scratch = m_run.scratchRoot();
    m_thread = QThread::create([this, dbPath, registry, scratch] {
        RecoveryReport report;
        {
            Database::ScopedConnection conn(dbPath, QStringLiteral("recovery"));
            if (conn.isOpen())
                report = recoverRuns(conn.db(), *registry, scratch);
            else
                report.errors << conn.error();
        }
        QMetaObject::invokeMethod(this, [this, report] { finishRecovery(report); }, Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
    emit recoveringChanged();
    // Normally over in milliseconds. Only past a second does the window say so.
    m_slow = false;
    QTimer::singleShot(kSlowRecoveryMs, this, [this] {
        if (m_thread && !m_slow) {
            m_slow = true;
            emit recoveringChanged();
        }
    });
}

void StartupController::finishRecovery(const RecoveryReport &report)
{
    m_thread = nullptr;
    m_slow = false;
    m_lock.reset();  // unlocks, whatever happened

    QStringList lines;
    for (const RecoveryNotice &n : report.notices) {
        if (n.status == RunStatus::Recovered) {
            const QString what = n.recoveredTestIds.isEmpty() ? tr("Run") : m_run.testName(n.recoveredTestIds.first());
            lines << tr("%1 run recovered: %2").arg(what, hoursMinutes(n.recoveredDurationMs));
        } else {
            lines << tr("The run from %1 was interrupted").arg(QLocale().toString(n.startedAt.toLocalTime().time(), QLocale::ShortFormat));
        }
    }
    // Once per distinct problem: the same leftover failing again on every pass
    // (each location change) must not stack up identical lines.
    bool newProblem = false;
    for (const QString &e : report.errors) {
        if (!m_seenProblems.contains(e)) {
            m_seenProblems.insert(e);
            newProblem = true;
        }
    }
    if (newProblem)
        lines << tr("Some leftovers couldn't be cleaned up; see the log.");
    if (!lines.isEmpty()) {
        // Append to a notice nobody has dismissed yet; never replace it with less.
        const QString text = lines.join(QLatin1Char('\n'));
        m_notice = m_notice.isEmpty() ? text : m_notice + QLatin1Char('\n') + text;
        emit noticeChanged();
    }
    if (!report.notices.isEmpty())
        emit m_storage.historyChanged();
    emit recoveringChanged();
    emit recoveryFinished();
    if (m_again)
        recover();
}

void StartupController::setInstanceWarning(const QString &w)
{
    if (w == m_instanceWarning)
        return;
    m_instanceWarning = w;
    emit instanceWarningChanged();
}

void StartupController::dismissRecoveryNotice()
{
    if (m_notice.isEmpty())
        return;
    m_notice.clear();
    emit noticeChanged();
}

bool StartupController::firstRunNeeded() const
{
    return !m_settings.firstRunDone();
}

bool StartupController::hardwareReady() const
{
    return !m_run.probing();
}

QVariantList StartupController::hardwareCheck() const
{
    return m_run.hardwareCheck();
}

void StartupController::finishFirstRun()
{
    m_settings.setFirstRunDone(true);
}

} // namespace sb
