// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/enums.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QStringList>
#include <QUuid>
#include <QVector>

namespace sb {

class Registry;

// One line for the one-time startup notice ("The run from 14:02 was
// interrupted" / "Battery run recovered: 6 h 42 m"; Phase 8 owns the wording).
struct RecoveryNotice {
    QUuid runId;
    QDateTime startedAt;
    RunStatus status = RunStatus::Interrupted;  // Interrupted or Recovered
    QStringList recoveredTestIds;               // tests whose recover() produced a result
    qint64 recoveredDurationMs = 0;             // the longest recovered result's duration
};

struct RecoveryReport {
    QVector<RecoveryNotice> notices;
    QStringList errors;
    int scratchPathsRemoved = 0;
};

// Startup recovery (§3), run before the main UI while Storage is busy:
//  1. every run still 'running' was cut short (crash, power-off, kill). For
//     each of its unfinished tests (the "Queued" placeholders): a test with
//     RecoveryMode::FromCheckpoints gets recover() with its options and
//     checkpoints, and the result goes through the usual result rules; any
//     other test is marked Interrupted. The run becomes 'recovered' if some
//     recover() produced a real result (not Interrupted), else 'interrupted';
//     its overall score is recomputed and finished_at is its last sign of life.
//  2. F5: every scratch folder still registered for a run that isn't running
//     is deleted (that includes abandoned runs, which are already
//     'interrupted'), then its row. A path that isn't exactly
//     <scratchRoot>/syntheon-<uuid>/<name>, or is a link, is never deleted,
//     only unregistered.
// Each run is finalised last, so recovery interrupted half-way just runs
// again at the next start.
// `scratchRoot` is the orchestrator's (Config::scratchRoot, default
// QDir::tempPath()): only folders under <scratchRoot>/syntheon-<uuid>/ are
// ever deleted, and never through a link (AP5-R2-2).
RecoveryReport recoverRuns(QSqlDatabase db, const Registry &registry, const QString &scratchRoot = QString());

} // namespace sb
