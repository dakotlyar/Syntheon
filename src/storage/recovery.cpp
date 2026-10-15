// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/recovery.h"

#include "core/fs_util.h"
#include "core/itest.h"
#include "core/registry.h"
#include "core/result_rules.h"
#include "storage/run_repository.h"

#include <QCborValue>
#include <QDir>
#include <QFileInfo>

#include <exception>

namespace sb {

namespace {

const QString kClosedReason = QStringLiteral("SynBench closed before this test finished");

class StoredRecoveryContext : public RecoveryContext
{
public:
    StoredRecoveryContext(OptionValues options, QList<QCborMap> checkpoints, SystemSnapshot system)
        : m_options(std::move(options)), m_checkpoints(std::move(checkpoints)), m_system(std::move(system))
    {
    }
    const OptionValues &options() const override { return m_options; }
    QList<QCborMap> checkpoints() const override { return m_checkpoints; }
    const SystemSnapshot &system() const override { return m_system; }

private:
    OptionValues m_options;
    QList<QCborMap> m_checkpoints;
    SystemSnapshot m_system;
};

bool unfinished(const TestResult &t)
{
    return t.status == Status::NotRun;
}

void recoverRun(RunRepository &repo, const Registry &registry, const QUuid &uuid, RecoveryReport &report)
{
    const auto runId = repo.runId(uuid);
    auto run = repo.load(uuid);
    if (!runId || !run) {
        report.errors.append(QStringLiteral("Couldn't load run %1: %2").arg(uuid.toString(), repo.lastError()));
        return;
    }

    // AP5-R2-3: the last sign of life is read before anything is written,
    // and every write for this run happens in one transaction at the end, so
    // a recovery cut short (crash, power loss) leaves the run 'running' and
    // untouched, and the next start produces the same final state.
    QDateTime finished = repo.lastActivity(*runId);
    if (!finished.isValid())
        finished = run->startedAt;

    QVector<LogLine> log;
    const auto note = [&](LogLevel level, const QString &testId, const QString &message) {
        log.append({QDateTime::currentDateTimeUtc(), level, testId, message});
    };

    QVector<std::pair<int, TestResult>> replaced;
    for (int i = 0; i < run->tests.size(); ++i) {
        const TestResult &stored = run->tests[i];
        if (!unfinished(stored))
            continue;
        ITest *test = registry.find(stored.testId);

        TestResult r;
        if (test && test->info().recovery == RecoveryMode::FromCheckpoints) {
            QList<QCborMap> checkpoints;
            for (const QByteArray &bytes : repo.loadCheckpoints(*runId, stored.testId))
                checkpoints.append(QCborValue::fromCbor(bytes).toMap());
            StoredRecoveryContext ctx(stored.options, checkpoints, run->system);
            try {
                r = test->recover(ctx);
            } catch (const std::exception &ex) {
                r = TestResult::error(QStringLiteral("Recovery failed: %1").arg(QString::fromLocal8Bit(ex.what())));
            } catch (...) {
                r = TestResult::error(QStringLiteral("Recovery failed"));
            }
            r.testId = stored.testId;
            r.testVersion = test->info().testVersion;
            r.formulaVersion = test->info().formulaVersion;
            r.options = stored.options;
            applyResultRules(*test, r, [&](const QString &m) { note(LogLevel::Error, stored.testId, m); });
            if (r.status != Status::Interrupted)
                note(LogLevel::Info, stored.testId,
                     QStringLiteral("Recovered at startup from %1 checkpoints").arg(checkpoints.size()));
        } else {
            r = stored;
            r.status = Status::Interrupted;
            r.statusReason = kClosedReason;
            r.score.reset();
        }
        replaced.append({i, r});
        run->tests[i] = r;
    }

    // "Recovered" comes from the final rows, not from what this pass did:
    // a recoverable test holding a real result (anything but NotRun /
    // Interrupted) makes the run recovered.
    RecoveryNotice notice;
    notice.runId = uuid;
    notice.startedAt = run->startedAt;
    for (const TestResult &t : std::as_const(run->tests)) {
        const ITest *test = registry.find(t.testId);
        if (test && test->info().recovery == RecoveryMode::FromCheckpoints && t.status != Status::NotRun
            && t.status != Status::Interrupted) {
            notice.recoveredTestIds.append(t.testId);
            notice.recoveredDurationMs = std::max(notice.recoveredDurationMs, t.durationMs);
        }
    }
    notice.status = notice.recoveredTestIds.isEmpty() ? RunStatus::Interrupted : RunStatus::Recovered;
    note(notice.status == RunStatus::Recovered ? LogLevel::Info : LogLevel::Warning, {},
         notice.status == RunStatus::Recovered ? QStringLiteral("Run recovered at startup")
                                               : QStringLiteral("Run interrupted: SynBench closed while it was running"));
    const auto overall = computeOverall(run->tests, [&](const QString &id) {
        const ITest *t = registry.find(id);
        return t && t->info().countsTowardOverall;
    });

    if (!repo.finalizeRun(*runId, replaced, log, notice.status, finished, overall)) {
        report.errors.append(repo.lastError());  // still 'running': retried next start
        return;
    }
    report.notices.append(notice);
}

void cleanScratch(RunRepository &repo, const QString &scratchRoot, RecoveryReport &report)
{
    for (const RunRepository::ScratchPath &s : repo.scratchPathsOfFinishedRuns()) {
        const QFileInfo info(s.path);
        if (!fs::isSafeScratchPath(s.path, scratchRoot, s.runUuid)) {
            // The database is a file: a damaged or edited one, or a test that
            // swapped its folder for a link, must not be able to point this
            // at anything else.
            report.errors.append(QStringLiteral("Ignored an unexpected scratch path: %1").arg(s.path));
        } else if (!fs::removeTreeNoFollow(s.path)) {
            report.errors.append(QStringLiteral("Couldn't delete the scratch folder %1").arg(s.path));
            continue;  // keep the row; try again next start
        } else {
            QDir().rmdir(info.path());  // the run's folder, once empty
            ++report.scratchPathsRemoved;
        }
        if (!repo.deleteScratchPathRow(s.id))
            report.errors.append(repo.lastError());
    }
}

} // namespace

RecoveryReport recoverRuns(QSqlDatabase db, const Registry &registry, const QString &scratchRoot)
{
    RecoveryReport report;
    RunRepository repo(db);
    for (const QUuid &uuid : repo.runsWithStatus(RunStatus::Running))
        recoverRun(repo, registry, uuid, report);
    cleanScratch(repo, scratchRoot.isEmpty() ? QDir::tempPath() : scratchRoot, report);
    return report;
}

} // namespace sb
