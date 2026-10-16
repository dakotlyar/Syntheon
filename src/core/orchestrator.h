// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/log_line.h"
#include "core/options.h"
#include "core/result.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <QVector>

#include <chrono>
#include <memory>
#include <thread>

namespace sb {

class PowerInhibitor;
class PrivilegedOps;
class Registry;
class RunSink;

struct PlanItem {
    QString testId;
    OptionValues options;
};
using Plan = QVector<PlanItem>;

struct RunMeta {
    QString appVersion;  // "2026.11.0"
    QString appBuild;    // "linux-x64 abc1234"
    SystemSnapshot system;
};

// Live progress for the queue row of test `index` (canonical plan position).
struct ProgressUpdate {
    int index = -1;
    QString testId;
    double fraction = 0.0;
    QString stage;
};

// Runs a plan, one test at a time, on a single worker thread (P5, D2), and
// writes it to a RunSink as it goes (§3):
//  - tests run in canonical order (P19), whatever order the plan lists them;
//  - each result is committed as soon as its test finishes, log lines are
//    batched about every 500 ms;
//  - Stop cancels the current test and records the queued ones as NotRun; the
//    run is saved as Stopped (D3);
//  - at a test's timeLimit the watchdog cancels it as TimedOut; a test that
//    doesn't return within the grace period after any cancel is abandoned:
//    the run is saved as Interrupted and the orchestrator needs a restart
//    (a thread can't be killed safely, R7);
//  - an exception from run() becomes Status::Error and the run continues;
//  - system sleep is blocked for the whole run (D8).
// Lives on the UI thread; every signal is emitted there.
class Orchestrator : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Running,
        Stopping,
        Abandoned,  // a test didn't respond; restart SynBench to run again
    };
    Q_ENUM(State)

    struct Config {
        std::chrono::milliseconds grace{10000};
        std::chrono::milliseconds watchdogInterval{100};
        std::chrono::milliseconds logFlushInterval{500};
        std::chrono::milliseconds progressInterval{100};  // ~10 Hz to the UI
        QString scratchRoot;                              // default: QDir::tempPath()
        // Handed to tests via RunContext/ProbeContext::privileged(). Must
        // outlive the orchestrator; null = every privileged call fails with
        // Unavailable.
        PrivilegedOps *privileged = nullptr;
    };

    struct StartResult {
        bool ok = false;
        QStringList errors;
        QUuid runId;
    };

    Orchestrator(const Registry &registry, RunSink &sink, PowerInhibitor &inhibitor, QObject *parent = nullptr);
    Orchestrator(const Registry &registry, RunSink &sink, PowerInhibitor &inhibitor, Config config,
                 QObject *parent = nullptr);
    // Stops a running run and waits for it (up to the grace period).
    ~Orchestrator() override;

    // Validates the plan (known tests, no duplicates, exclusivity, options,
    // preconditions) and starts it. Refused unless Idle.
    StartResult start(const Plan &plan, const RunMeta &meta);
    void stop();

    // True once the worker thread has left its run loop, including an
    // abandoned (hung) one that finally returned. A hung test can't be killed
    // (R7), so anything its run() uses (the registry, captured locals) must
    // outlive it: wait here before destroying them. The sink, inhibitor and
    // registry must otherwise outlive the Orchestrator, as before; the
    // destructor makes a late-returning worker leave them alone.
    bool waitForWorkerExit(std::chrono::milliseconds timeout);

    State state() const { return m_state; }
    // The run in progress (or the last one), updated as tests finish: what
    // "View results so far" shows.
    const RunRecord &currentRun() const { return m_run; }

signals:
    void stateChanged(sb::Orchestrator::State state);
    void runStarted(const sb::RunRecord &run, const QStringList &plannedTestIds);
    void testStarted(int index, const QString &testId);
    void progress(const sb::ProgressUpdate &update);
    void subTestChanged(int index, const QString &subTestId, bool finished, sb::Status status);
    void logLine(const sb::LogLine &line);
    void testFinished(int index, const sb::TestResult &result);
    void runFinished(const sb::RunRecord &run);
    // A write to the history failed (the run carries on).
    void storageError(const QString &message);

public:
    struct Shared;  // state shared with the worker thread (orchestrator.cpp)

private:
    void setState(State s);
    void onWatchdog();
    void abandon(int index);
    void onWorkerDone(const RunRecord &run);

    const Registry &m_registry;
    RunSink &m_sink;
    PowerInhibitor &m_inhibitor;
    Config m_config;
    State m_state = State::Idle;
    RunRecord m_run;
    std::shared_ptr<Shared> m_shared;
    std::thread m_worker;
    QTimer m_watchdog;
};

} // namespace sb
