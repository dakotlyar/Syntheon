// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/controllers/startup_controller.h"

#include "app/controllers/run_controller.h"
#include "app/controllers/settings_controller.h"
#include "app/controllers/storage_controller.h"
#include "storage/recovery.h"
#include "storage/storage.h"

#include <QLocale>
#include <QStringList>

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

void StartupController::recover()
{
    if (!m_storage.storage().isOpen() || m_run.running() || m_run.hasRun())
        return;
    m_storage.setLocked(true, tr("Finishing a run that was cut short"));
    const RecoveryReport report = recoverRuns(m_storage.storage().connection(), m_run.registry(), m_run.scratchRoot());
    m_storage.setLocked(false);

    QStringList lines;
    for (const RecoveryNotice &n : report.notices) {
        if (n.status == RunStatus::Recovered) {
            QString what = n.recoveredTestIds.isEmpty() ? tr("Run") : m_run.testName(n.recoveredTestIds.first());
            lines << tr("%1 run recovered: %2").arg(what, hoursMinutes(n.recoveredDurationMs));
        } else {
            lines << tr("The run from %1 was interrupted").arg(QLocale().toString(n.startedAt.toLocalTime().time(), QLocale::ShortFormat));
        }
    }
    if (!report.errors.isEmpty())
        lines << tr("Some leftovers couldn't be cleaned up; see the log.");
    const QString text = lines.join(QLatin1Char('\n'));
    if (text != m_notice) {
        m_notice = text;
        emit noticeChanged();
    }
    if (!report.notices.isEmpty())
        emit m_storage.historyChanged();
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
