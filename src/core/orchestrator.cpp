// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/orchestrator.h"

#include "core/itest.h"
#include "core/power_inhibitor.h"
#include "core/registry.h"
#include "core/result_rules.h"
#include "core/run_sink.h"

#include <QCborValue>
#include <QDir>
#include <QMutex>
#include <QSet>
#include <QDebug>
#include <QFileInfo>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <memory>

namespace sb {

using Clock = std::chrono::steady_clock;

namespace {

const QString kDidNotRespond = QStringLiteral("Did not respond, restart SynBench");

UnavailablePrivilegedOps &unavailableOps()
{
    static UnavailablePrivilegedOps ops;
    return ops;
}

class SnapshotProbeContext : public ProbeContext
{
public:
    SnapshotProbeContext(const SystemSnapshot &s, PrivilegedOps *ops) : m_system(s), m_ops(ops) {}
    const SystemSnapshot &system() const override { return m_system; }
    PrivilegedOps &privileged() const override { return m_ops ? *m_ops : unavailableOps(); }

private:
    const SystemSnapshot &m_system;
    PrivilegedOps *m_ops;
};

} // namespace

// Everything the worker thread shares with the orchestrator. Owned by a
// shared_ptr so an abandoned (hung) worker can never touch freed memory.
struct Orchestrator::Shared {
    struct Entry {
        ITest *test = nullptr;
        QString testId;
        OptionValues options;
    };
    enum class Cancel { None, Stopped, TimedOut };

    Shared(const Config &c, RunSink &s, PowerInhibitor &i) : config(c), sink(s), inhibitor(i) {}

    const Config config;
    RunSink &sink;
    PowerInhibitor &inhibitor;
    QVector<Entry> plan;
    RunRecord header;  // immutable after start

    QMutex mutex;  // guards the members below
    Orchestrator *receiver = nullptr;
    RunRecord run;  // the worker's view; tests appended as they finish
    bool stopRequested = false;
    Cancel cancel = Cancel::None;
    int current = -1;
    bool testRunning = false;
    Clock::time_point testStart, cancelAt;
    std::chrono::seconds limit{0};
    QVector<LogLine> logBuffer;
    Clock::time_point lastFlush = Clock::now();
    Clock::time_point lastProgress;
    QString lastStage;

    // F6: read from any thread (log/progress), written once by the worker.
    std::atomic<std::thread::id> workerId{};
    std::atomic<bool> cancelFlag{false};
    std::atomic<bool> abandoned{false};
    std::atomic<bool> workerExited{false};

    // Serialises sink calls. abandon() holds it while it finalises the run,
    // so a late worker write can't interleave.
    QMutex sinkMutex;

    // Runs `f` on the orchestrator's thread, unless the run was abandoned.
    template <typename F>
    void post(F &&f)
    {
        QMutexLocker lock(&mutex);
        if (!receiver)
            return;
        std::weak_ptr<Shared> weak = self;
        QMetaObject::invokeMethod(
            receiver,
            [weak, r = receiver, f = std::forward<F>(f)]() mutable {
                const auto s = weak.lock();
                if (s && !s->abandoned)
                    f(r);
            },
            Qt::QueuedConnection);
    }

    // Calls the sink unless the run was abandoned; reports failures.
    template <typename F>
    bool sinkCall(F &&f)
    {
        QMutexLocker lock(&sinkMutex);
        if (abandoned)
            return false;
        QString error;
        if (f(error))
            return true;
        qWarning("SynBench: history write failed: %s", qPrintable(error));
        post([error](Orchestrator *o) { emit o->storageError(error); });
        return false;
    }

    bool onWorker() const { return std::this_thread::get_id() == workerId; }

    void log(LogLevel level, const QString &testId, const QString &message)
    {
        const LogLine line{QDateTime::currentDateTimeUtc(), level, testId, message};
        {
            QMutexLocker lock(&mutex);
            if (abandoned)
                return;
            logBuffer.append(line);
        }
        post([line](Orchestrator *o) { emit o->logLine(line); });
        if (onWorker())
            flushLogs(false);
    }

    // Worker thread only: writes buffered log lines, at most every
    // logFlushInterval unless forced.
    void flushLogs(bool force)
    {
        QVector<LogLine> lines;
        {
            QMutexLocker lock(&mutex);
            const auto now = Clock::now();
            if (!force && now - lastFlush < config.logFlushInterval)
                return;
            lastFlush = now;
            lines.swap(logBuffer);
        }
        if (lines.isEmpty())
            return;
        sinkCall([&](QString &e) { return sink.appendLog(header.uuid, lines, &e); });
    }

    void progress(int index, const QString &testId, double fraction, const QString &stage)
    {
        fraction = std::clamp(fraction, 0.0, 1.0);
        {
            QMutexLocker lock(&mutex);
            if (abandoned)
                return;
            const auto now = Clock::now();
            if (stage == lastStage && fraction < 1.0 && now - lastProgress < config.progressInterval)
                return;
            lastProgress = now;
            lastStage = stage;
        }
        const ProgressUpdate u{index, testId, fraction, stage};
        post([u](Orchestrator *o) { emit o->progress(u); });
        if (onWorker())
            flushLogs(false);
    }

    // Defined below (they need the helpers in the anonymous namespace).
    void commit(int index, const TestResult &r);
    static void workerMain(std::shared_ptr<Shared> s);

    std::weak_ptr<Shared> self;
};

namespace {

using Shared = Orchestrator::Shared;

// The test's view of the run (§2.7).
class WorkerRunContext : public RunContext
{
public:
    WorkerRunContext(Shared &s, int index, const QString &testId) : m_s(s), m_index(index), m_testId(testId) {}

    void progress(double fraction, const QString &stage) override { m_s.progress(m_index, m_testId, fraction, stage); }
    void log(LogLevel level, const QString &message) override { m_s.log(level, m_testId, message); }
    bool cancelled() const override { return m_s.cancelFlag.load(); }

    void subTestBegin(const QString &id) override
    {
        const int index = m_index;
        m_s.post([index, id](Orchestrator *o) { emit o->subTestChanged(index, id, false, Status::Ok); });
    }
    void subTestEnd(const QString &id, Status status) override
    {
        const int index = m_index;
        m_s.post([index, id, status](Orchestrator *o) { emit o->subTestChanged(index, id, true, status); });
    }

    void checkpoint(const QCborMap &payload) override
    {
        const int seq = m_checkpointSeq++;
        const QByteArray bytes = QCborValue(payload).toCbor();
        m_s.sinkCall([&](QString &e) { return m_s.sink.addCheckpoint(m_s.header.uuid, m_testId, seq, bytes, &e); });
    }

    QString scratchDir() override
    {
        if (!m_scratch.isEmpty())
            return m_scratch;
        const QString root = m_s.config.scratchRoot.isEmpty() ? QDir::tempPath() : m_s.config.scratchRoot;
        const QString path = QDir(root).filePath(
            QStringLiteral("synbench-%1/%2").arg(m_s.header.uuid.toString(QUuid::WithoutBraces), m_testId));
        if (!QDir().mkpath(path)) {
            m_s.log(LogLevel::Error, m_testId, QStringLiteral("Couldn't create the scratch folder %1").arg(path));
            return {};
        }
        m_s.sinkCall([&](QString &e) { return m_s.sink.addScratchPath(m_s.header.uuid, path, &e); });
        m_scratch = path;
        return m_scratch;
    }

    const SystemSnapshot &system() const override { return m_s.header.system; }
    PrivilegedOps &privileged() override
    {
        return m_s.config.privileged ? *m_s.config.privileged : unavailableOps();
    }

    // After run() returns: delete the scratch folder and its registration.
    void cleanup()
    {
        if (m_scratch.isEmpty())
            return;
        if (!QDir(m_scratch).removeRecursively())
            m_s.log(LogLevel::Warning, m_testId, QStringLiteral("Couldn't delete the scratch folder %1").arg(m_scratch));
        m_s.sinkCall([&](QString &e) { return m_s.sink.removeScratchPath(m_s.header.uuid, m_scratch, &e); });
        QDir().rmdir(QFileInfo(m_scratch).path());  // the run's folder, once empty
    }

private:
    Shared &m_s;
    const int m_index;
    const QString m_testId;
    int m_checkpointSeq = 0;
    QString m_scratch;
};

TestResult notRun(const Shared::Entry &e, const QString &reason)
{
    TestResult r = TestResult::withStatus(Status::NotRun, reason);
    r.testId = e.testId;
    r.testVersion = e.test->info().testVersion;
    r.formulaVersion = e.test->info().formulaVersion;
    r.options = e.options;
    return r;
}

std::optional<double> overallOf(const Shared &s, const QVector<TestResult> &tests)
{
    return computeOverall(tests, [&s](const QString &id) {
        for (const auto &e : s.plan) {
            if (e.testId == id)
                return e.test->info().countsTowardOverall;
        }
        return false;
    });
}

// Fills in what the orchestrator owns (identity, versions, options, timing),
// maps a cancel caused by the time limit to TimedOut, and scores the result.
void finishResult(Shared &s, const Shared::Entry &e, Shared::Cancel cancel, const QDateTime &started,
                  qint64 durationMs, TestResult &r)
{
    const TestInfo &info = e.test->info();
    r.testId = e.testId;
    r.testVersion = info.testVersion;
    r.formulaVersion = info.formulaVersion;
    r.options = e.options;
    r.startedAt = started;
    r.durationMs = durationMs;

    if (cancel == Shared::Cancel::TimedOut
        && (r.status == Status::Ok || r.status == Status::Partial || r.status == Status::Cancelled)) {
        r.status = Status::TimedOut;
        r.statusReason = QStringLiteral("Exceeded its time limit of %1 s").arg(e.test->timeLimit(e.options).count());
    } else if (cancel == Shared::Cancel::Stopped && r.status == Status::Cancelled && r.statusReason.isEmpty()) {
        r.statusReason = QStringLiteral("Stopped");
    }

    applyResultRules(*e.test, r, [&](const QString &message) { s.log(LogLevel::Error, e.testId, message); });
}

} // namespace

void Orchestrator::Shared::commit(int index, const TestResult &r)
{
    sinkCall([&](QString &e) { return sink.commitTest(header.uuid, index, r, &e); });
    {
        QMutexLocker lock(&mutex);
        run.tests.append(r);
    }
    post([index, r](Orchestrator *o) {
        o->m_run.tests.append(r);
        emit o->testFinished(index, r);
    });
}

void Orchestrator::Shared::workerMain(std::shared_ptr<Shared> s)
{
    s->workerId = std::this_thread::get_id();

    QString error;
    if (!s->inhibitor.acquire(QStringLiteral("Benchmark running"), &error))
        s->log(LogLevel::Warning, {}, QStringLiteral("Couldn't block system sleep: %1").arg(error));

    RunRecord planned = s->header;
    for (const Shared::Entry &e : std::as_const(s->plan))
        planned.tests.append(notRun(e, QStringLiteral("Queued")));
    const bool begun = s->sinkCall([&](QString &e) { return s->sink.beginRun(planned, &e); });
    if (begun)
        s->log(LogLevel::Info, {}, QStringLiteral("Run started"));
    else
        s->log(LogLevel::Error, {}, QStringLiteral("Couldn't save the run to the history; no tests were run"));

    int next = 0;
    for (; begun && next < s->plan.size(); ++next) {
        const Shared::Entry &e = s->plan[next];
        {
            QMutexLocker lock(&s->mutex);
            if (s->stopRequested)
                break;
            s->current = next;
            s->testRunning = true;
            s->testStart = Clock::now();
            s->limit = e.test->timeLimit(e.options);
            s->cancel = Shared::Cancel::None;
            s->cancelFlag = false;
            s->lastStage.clear();
        }
        const int index = next;
        const QString testId = e.testId;
        s->post([index, testId](Orchestrator *o) { emit o->testStarted(index, testId); });
        s->log(LogLevel::Info, e.testId, QStringLiteral("Starting %1").arg(e.test->info().name));

        const bool durable = e.test->info().recovery == RecoveryMode::FromCheckpoints;
        if (durable) {
            QMutexLocker lock(&s->sinkMutex);
            s->sink.setDurable(true);
        }

        WorkerRunContext ctx(*s, index, e.testId);
        const QDateTime started = QDateTime::currentDateTimeUtc();
        const auto t0 = Clock::now();
        TestResult r;
        try {
            r = e.test->run(ctx, e.options);
        } catch (const std::exception &ex) {
            r = TestResult::error(QString::fromLocal8Bit(ex.what()));
        } catch (...) {
            r = TestResult::error(QStringLiteral("Unknown error"));
        }
        const qint64 durationMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();

        Shared::Cancel cancel;
        {
            QMutexLocker lock(&s->mutex);
            if (s->abandoned) {
                // Too late: the run was already finalised without this test.
                lock.unlock();
                {
                    QMutexLocker sinkLock(&s->sinkMutex);
                    s->sink.threadFinished();
                }
                // F7: on Windows the sleep block belongs to this thread
                // (SetThreadExecutionState), so only this thread can lift it;
                // abandon()'s release from the UI thread can't. Until a hung
                // test returns (or the app exits), sleep stays blocked.
                s->inhibitor.release();
                s->workerExited = true;
                return;
            }
            s->testRunning = false;
            cancel = s->cancel;
        }

        finishResult(*s, e, cancel, started, durationMs, r);
        ctx.cleanup();
        if (durable) {
            QMutexLocker lock(&s->sinkMutex);
            s->sink.setDurable(false);
        }
        s->log(r.status == Status::Error ? LogLevel::Error : LogLevel::Info, e.testId,
               r.statusReason.isEmpty()
                   ? QStringLiteral("%1 finished: %2").arg(e.test->info().name, toKey(r.status))
                   : QStringLiteral("%1 finished: %2 (%3)").arg(e.test->info().name, toKey(r.status), r.statusReason));
        s->flushLogs(true);
        s->commit(index, r);
    }

    bool stopped;
    {
        QMutexLocker lock(&s->mutex);
        stopped = s->stopRequested;
    }
    for (; next < s->plan.size(); ++next) {
        const QString reason = !begun ? QStringLiteral("Not run: the history couldn't be written")
                                      : QStringLiteral("Not run: the run was stopped");
        s->commit(next, notRun(s->plan[next], reason));
    }

    RunRecord record;
    {
        QMutexLocker lock(&s->mutex);
        record = s->run;
    }
    record.status = !begun ? RunStatus::Interrupted : stopped ? RunStatus::Stopped : RunStatus::Completed;
    record.finishedAt = QDateTime::currentDateTimeUtc();
    record.overall = overallOf(*s, record.tests);
    s->log(LogLevel::Info, {}, QStringLiteral("Run %1").arg(toKey(record.status)));
    s->flushLogs(true);
    if (begun)
        s->sinkCall([&](QString &e) { return s->sink.finishRun(record, &e); });
    s->inhibitor.release();
    {
        QMutexLocker lock(&s->sinkMutex);
        s->sink.threadFinished();
    }
    s->post([record](Orchestrator *o) { o->onWorkerDone(record); });
    s->workerExited = true;
}

Orchestrator::Orchestrator(const Registry &registry, RunSink &sink, PowerInhibitor &inhibitor, QObject *parent)
    : Orchestrator(registry, sink, inhibitor, Config{}, parent)
{
}

Orchestrator::Orchestrator(const Registry &registry, RunSink &sink, PowerInhibitor &inhibitor, Config config,
                           QObject *parent)
    : QObject(parent), m_registry(registry), m_sink(sink), m_inhibitor(inhibitor), m_config(std::move(config))
{
    m_watchdog.setInterval(m_config.watchdogInterval);
    connect(&m_watchdog, &QTimer::timeout, this, &Orchestrator::onWatchdog);
}

Orchestrator::~Orchestrator()
{
    if (m_shared && (m_state == State::Running || m_state == State::Stopping)) {
        stop();
        const auto deadline = Clock::now() + m_config.grace;
        while (!m_shared->workerExited && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!m_shared->workerExited) {
            int index;
            {
                QMutexLocker lock(&m_shared->mutex);
                index = m_shared->current;
            }
            abandon(index);
        }
    }
    bool exited = true;
    if (m_shared) {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->receiver = nullptr;
        exited = m_shared->workerExited;
    }
    // A worker still stuck somewhere abandon() couldn't claim (outside a
    // test) is left behind rather than blocking shutdown forever. It only
    // holds the shared state, which it co-owns.
    if (m_worker.joinable()) {
        if (exited || m_state == State::Idle)
            m_worker.join();
        else
            m_worker.detach();
    }
}

void Orchestrator::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(s);
}

Orchestrator::StartResult Orchestrator::start(const Plan &plan, const RunMeta &meta)
{
    StartResult result;
    if (m_state == State::Abandoned) {
        result.errors.append(QStringLiteral("A test stopped responding. Restart SynBench to run tests again."));
        return result;
    }
    if (m_state != State::Idle) {
        result.errors.append(QStringLiteral("A run is already in progress."));
        return result;
    }
    if (plan.isEmpty()) {
        result.errors.append(QStringLiteral("No tests selected."));
        return result;
    }

    // Validate, then sort into canonical order (P19).
    QVector<Shared::Entry> entries;
    QSet<QString> seen;
    bool runsAlone = false;
    for (const PlanItem &item : plan) {
        ITest *test = m_registry.find(item.testId);
        if (!test) {
            result.errors.append(QStringLiteral("Unknown test \"%1\".").arg(item.testId));
            continue;
        }
        if (seen.contains(item.testId)) {
            result.errors.append(QStringLiteral("%1 is in the plan twice.").arg(test->info().name));
            continue;
        }
        seen.insert(item.testId);
        runsAlone = runsAlone || test->info().exclusivity == Exclusivity::RunsAlone;
        entries.append({test, item.testId, item.options});
    }
    if (runsAlone && entries.size() > 1) {
        for (const auto &e : entries) {
            if (e.test->info().exclusivity == Exclusivity::RunsAlone)
                result.errors.append(QStringLiteral("%1 always runs on its own.").arg(e.test->info().name));
        }
    }
    const SnapshotProbeContext probe(meta.system, m_config.privileged);
    for (const auto &e : entries) {
        for (const ValidationError &v : e.test->validate(e.options).errors)
            result.errors.append(QStringLiteral("%1: %2").arg(e.test->info().name, v.message));
        for (const Precondition &p : e.test->preconditions(e.options, probe)) {
            if (p.state != Precondition::State::NotMet)
                continue;
            result.errors.append(p.detail.isEmpty()
                                     ? QStringLiteral("%1: %2").arg(e.test->info().name, p.label)
                                     : QStringLiteral("%1: %2 (%3)").arg(e.test->info().name, p.label, p.detail));
        }
    }
    if (!result.errors.isEmpty())
        return result;

    std::sort(entries.begin(), entries.end(), [this](const Shared::Entry &a, const Shared::Entry &b) {
        return m_registry.indexOf(a.testId) < m_registry.indexOf(b.testId);
    });

    RunRecord header;
    header.uuid = QUuid::createUuid();
    header.kind = runsAlone ? RunKind::Exclusive : RunKind::Standard;
    header.status = RunStatus::Running;
    header.startedAt = QDateTime::currentDateTimeUtc();
    header.appVersion = meta.appVersion;
    header.appBuild = meta.appBuild;
    header.system = meta.system;

    auto shared = std::make_shared<Shared>(m_config, m_sink, m_inhibitor);
    shared->self = shared;
    shared->plan = entries;
    shared->header = header;
    shared->run = header;
    shared->receiver = this;

    if (m_worker.joinable())
        m_worker.join();
    m_shared = shared;
    m_run = header;

    QStringList ids;
    for (const auto &e : entries)
        ids.append(e.testId);

    setState(State::Running);
    emit runStarted(m_run, ids);
    m_watchdog.start();
    m_worker = std::thread(&Shared::workerMain, shared);

    result.ok = true;
    result.runId = header.uuid;
    return result;
}

void Orchestrator::stop()
{
    if (m_state != State::Running || !m_shared)
        return;
    {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->stopRequested = true;
        if (m_shared->testRunning && m_shared->cancel == Shared::Cancel::None) {
            m_shared->cancel = Shared::Cancel::Stopped;
            m_shared->cancelAt = Clock::now();
            m_shared->cancelFlag = true;
        }
    }
    m_shared->log(LogLevel::Info, {}, QStringLiteral("Stop requested"));
    setState(State::Stopping);
}

void Orchestrator::onWatchdog()
{
    const auto s = m_shared;
    if (!s)
        return;
    bool timedOut = false;
    bool hung = false;
    int index = -1;
    QString testId;
    std::chrono::seconds limit{0};
    {
        QMutexLocker lock(&s->mutex);
        if (!s->testRunning)
            return;
        const auto now = Clock::now();
        index = s->current;
        testId = s->plan[index].testId;
        limit = s->limit;
        if (s->cancel == Shared::Cancel::None && limit.count() > 0 && now - s->testStart >= limit) {
            s->cancel = Shared::Cancel::TimedOut;
            s->cancelAt = now;
            s->cancelFlag = true;
            timedOut = true;
        }
        hung = s->cancel != Shared::Cancel::None && now - s->cancelAt >= m_config.grace;
    }
    if (timedOut)
        s->log(LogLevel::Warning, testId, QStringLiteral("Reached its time limit of %1 s; cancelling").arg(limit.count()));
    if (hung)
        abandon(index);
}

// The current test ignored a cancel for the whole grace period. A thread can't
// be killed safely (R7), so the run is finalised here as Interrupted and the
// worker is left behind; the orchestrator refuses new runs until a restart.
//
// F10 (accepted as R7): the abandoned thread keeps running the test's code
// for as long as it likes, using CPU and possibly the scratch folder. Nothing
// can stop it short of process exit; the out-of-process runner (D2) is the
// real fix, when a test first needs it.
void Orchestrator::abandon(int index)
{
    const auto s = m_shared;
    if (!s)
        return;
    QMutexLocker sinkLock(&s->sinkMutex);
    RunRecord record;
    QVector<LogLine> pendingLog;
    {
        QMutexLocker lock(&s->mutex);
        // F4: the decision to abandon was taken outside this lock. In between,
        // the hung test may have returned and the next one started, so
        // re-check that it's still the same test, still cancelled, and still
        // past its grace period; otherwise a healthy test would be killed.
        if (!s->testRunning || s->current != index || s->cancel == Shared::Cancel::None
            || Clock::now() - s->cancelAt < m_config.grace)
            return;
        s->abandoned = true;
        s->receiver = nullptr;
        index = s->current;
        record = s->run;
        pendingLog.swap(s->logBuffer);
    }
    m_watchdog.stop();

    const Shared::Entry &hungEntry = s->plan[index];
    // Lines still in the buffer were already streamed to the UI; they only
    // need saving. The new line is both saved and streamed.
    const LogLine abandonedLine{QDateTime::currentDateTimeUtc(), LogLevel::Error, hungEntry.testId,
                                QStringLiteral("%1 did not respond after being cancelled; the run was abandoned")
                                    .arg(hungEntry.test->info().name)};
    pendingLog.append(abandonedLine);
    emit logLine(abandonedLine);

    TestResult hung = TestResult::interrupted(kDidNotRespond);
    hung.testId = hungEntry.testId;
    hung.testVersion = hungEntry.test->info().testVersion;
    hung.formulaVersion = hungEntry.test->info().formulaVersion;
    hung.options = hungEntry.options;
    QVector<std::pair<int, TestResult>> toCommit = {{index, hung}};
    for (int i = index + 1; i < s->plan.size(); ++i)
        toCommit.append({i, notRun(s->plan[i], QStringLiteral("Skipped: a test didn't respond"))});

    record.status = RunStatus::Interrupted;
    record.finishedAt = QDateTime::currentDateTimeUtc();
    for (const auto &[i, r] : toCommit)
        record.tests.append(r);
    record.overall = overallOf(*s, record.tests);

    // Finalise from this thread (the worker is stuck).
    QString error;
    const auto check = [&](bool ok) {
        if (!ok) {
            qWarning("SynBench: history write failed: %s", qPrintable(error));
            emit storageError(error);
        }
    };
    check(m_sink.appendLog(record.uuid, pendingLog, &error));
    for (const auto &[i, r] : toCommit)
        check(m_sink.commitTest(record.uuid, i, r, &error));
    check(m_sink.finishRun(record, &error));
    m_sink.threadFinished();
    m_inhibitor.release();

    if (m_worker.joinable())
        m_worker.detach();
    m_run = record;
    for (const auto &[i, r] : toCommit)
        emit testFinished(i, r);
    setState(State::Abandoned);
    emit runFinished(m_run);
}

void Orchestrator::onWorkerDone(const RunRecord &run)
{
    m_watchdog.stop();
    if (m_worker.joinable())
        m_worker.join();
    m_run = run;
    setState(State::Idle);
    emit runFinished(m_run);
}

} // namespace sb
