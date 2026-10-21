// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/orchestrator.h"
#include "core/registry.h"
#include "core/selection_model.h"
#include "storage/sqlite_run_sink.h"

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <QVector>
#include <QVariantList>

#include <map>
#include <memory>
#include <optional>

namespace sb {

class AppController;
class LogModel;
class OptionsModel;
class PowerInhibitor;
class RunQueueModel;
class Storage;
class StorageController;

// QML singleton "RunController": the Run screen (picker, option tabs,
// preconditions, estimate, run bar) and the live run behind the Progress
// screen (queue, log, Stop, ETA). Owns the registry, the selection, one
// OptionValues per test and the Orchestrator.
// The smoke run's verdict on one test's result (SYNBENCH_SMOKE, CI): Ok
// passes; so does a RAM result that is Partial only because loaded latency
// is N/A "needs at least two cores" (a one-core runner, §5.4; the release
// build has no hook to share the core). Anything else fails.
bool smokeAccepts(const TestResult &result);

class RunController : public QObject
{
    Q_OBJECT
    // --- Run screen ---
    Q_PROPERTY(QAbstractItemModel *catalog READ catalog CONSTANT)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(bool runsAloneSelected READ runsAloneSelected NOTIFY selectionChanged)
    Q_PROPERTY(QString estimateText READ estimateText NOTIFY estimateChanged)
    Q_PROPERTY(QString runButtonText READ runButtonText NOTIFY selectionChanged)
    Q_PROPERTY(bool canStart READ canStart NOTIFY startabilityChanged)
    Q_PROPERTY(QStringList problems READ problems NOTIFY startabilityChanged)
    // Why the last Start was refused (shown in the run bar); cleared by the next try or edit.
    Q_PROPERTY(QStringList startErrors READ lastStartErrors NOTIFY startErrorsChanged)
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY selectionChanged)
    Q_PROPERTY(QString activeTab READ activeTab WRITE setActiveTab NOTIFY activeTabChanged)
    Q_PROPERTY(QObject *options READ options NOTIFY activeTabChanged)
    Q_PROPERTY(QVariantList preconditions READ preconditions NOTIFY preconditionsChanged)
    Q_PROPERTY(bool probing READ probing NOTIFY probingChanged)
    // --- the live run ---
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool stopping READ stopping NOTIFY runningChanged)
    Q_PROPERTY(bool needsRestart READ needsRestart NOTIFY runningChanged)
    Q_PROPERTY(bool hasRun READ hasRun NOTIFY runChanged)
    Q_PROPERTY(QString runUuid READ runUuid NOTIFY runChanged)
    Q_PROPERTY(QString runTitle READ runTitle NOTIFY runChanged)
    Q_PROPERTY(QString runSubtitle READ runSubtitle NOTIFY etaChanged)
    Q_PROPERTY(QAbstractItemModel *queue READ queue CONSTANT)
    Q_PROPERTY(QAbstractItemModel *log READ log CONSTANT)
    Q_PROPERTY(QString currentStage READ currentStage NOTIFY currentChanged)
    Q_PROPERTY(QString currentTestColor READ currentTestColor NOTIFY currentChanged)
    // --- the held interface (Phase 4a P10): while a test draws in its own
    // window (the GPU test) the main window holds still ---
    Q_PROPERTY(bool interfaceHeld READ interfaceHeld NOTIFY interfaceHeldChanged)
    // M8: the live part on the held card ("Shading · DirectX 12"), set when a
    // held progress update's part differs from the last one. A part never repeats
    // the test's name: "Geometry · Vulkan" (the pill adds "GPU · ").
    Q_PROPERTY(QString heldPart READ heldPart NOTIFY heldPartChanged)
    // The held test's name ("GPU"), for the card's title; set before interfaceHeldChanged.
    Q_PROPERTY(QString heldTestName READ heldTestName NOTIFY interfaceHeldChanged)

public:
    RunController(Storage &storage, StorageController &storageController, AppController &app,
                  Orchestrator::Config config = {}, QObject *parent = nullptr);
    ~RunController() override;

    QAbstractItemModel *catalog() { return &m_selection; }
    SelectionModel &selection() { return m_selection; }
    Registry &registry() { return m_registry; }
    Orchestrator &orchestrator() { return m_orchestrator; }
    int selectedCount() const { return m_selection.selectedCount(); }
    bool runsAloneSelected() const { return m_selection.runsAloneSelected(); }
    QString estimateText() const;
    // "45 s", "4 min 30 s", "1 h 05 min" (the ETA's and the estimates' wording).
    static QString durationText(qint64 secs);
    QString runButtonText() const;
    bool canStart() const;
    QStringList problems() const;
    QVariantList tabs() const;
    QString activeTab() const { return m_activeTab; }
    void setActiveTab(const QString &id);
    QObject *options();
    QVariantList preconditions() const;
    bool probing() const { return m_probesPending > 0; }

    bool running() const;
    bool stopping() const { return m_orchestrator.state() == Orchestrator::State::Stopping; }
    bool needsRestart() const { return m_orchestrator.state() == Orchestrator::State::Abandoned; }
    bool hasRun() const { return !m_orchestrator.currentRun().uuid.isNull(); }
    QString runUuid() const;
    QString runTitle() const;
    QString runSubtitle() const;
    QAbstractItemModel *queue();
    QAbstractItemModel *log();
    QString currentStage() const { return m_stage; }
    QString currentTestColor() const { return m_stageColor; }
    bool interfaceHeld() const { return m_held; }
    QString heldPart() const { return m_heldPart; }
    QString heldTestName() const { return m_heldTestName; }
    // True only with the dev hook SYNBENCH_GPU_MINIMIZE_MAIN=1: the Progress
    // screen then minimises the main window while the interface is held.
    Q_INVOKABLE bool minimizeOnHold() const;

    // The in-progress or last run, for "View results so far".
    const RunRecord &currentRun() const { return m_orchestrator.currentRun(); }
    // The values one test would run with (tests, smoke runs).
    OptionValues valuesFor(const QString &testId);
    // Names for the log and the results (the registry's).
    QString testName(const QString &testId) const;
    QString testColor(const QString &testId) const;

    Q_INVOKABLE void toggle(const QString &testId);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void refreshProbes();
    Q_INVOKABLE void setOption(const QString &testId, const QString &key, const QVariant &value);
    // The first-run "hardware check": one row per test from its probe:
    // {id, name, colorToken, state: good|partial|na, detail, hint}.
    QVariantList hardwareCheck() const;
    QString scratchRoot() const { return m_scratchRoot; }

    Q_INVOKABLE QStringList lastStartErrors() const { return m_startErrors; }
    // "~24 s" / "~2 min" for a picker row, from the test's own estimate.
    Q_INVOKABLE QString estimateOf(const QString &testId);

    // SYNBENCH_SMOKE=1 (§10): runs every available stub, then Battery with
    // its preconditions overridden, and exits 0 if all of it worked.
    void runSmoke();

signals:
    void selectionChanged();
    void estimateChanged();
    void startabilityChanged();
    void activeTabChanged();
    void preconditionsChanged();
    void probingChanged();
    void runningChanged();
    void runChanged();
    void etaChanged();
    void currentChanged();
    void runStarted();
    void runFinished(const QString &uuid, const QString &status);
    void startRefused(const QStringList &errors);
    void startErrorsChanged();
    void runUpdated();  // a test finished: the current run changed
    void interfaceHeldChanged();
    void heldPartChanged();

private:
    struct TestState {
        Availability availability;
        bool probed = false;
        OptionValues values;
        std::unique_ptr<OptionsModel> model;
    };

    TestState *stateOf(const QString &id) const;
    void onSelectionChanged();
    void onProbeResult(const QString &id, const Availability &availability, const OptionSchema &schema);
    void updatePreconditions();
    OptionSchema effectiveSchema(const QString &id) const;
    void updateRunning();
    void refreshEta();
    // Stops/starts m_ticker; on release applies what was buffered, in order,
    // and emits etaChanged().
    // `replayProgress` false drops the buffered progress instead (a test that is
    // already finished: its row must keep its final text).
    void setHeld(bool held, bool replayProgress = true);
    void applyProgress(const ProgressUpdate &u);
    void connectOrchestrator();
    std::unique_ptr<ProbeContext> makeProbeContext() const;
    QStringList validationErrors() const;

    Storage &m_storage;
    StorageController &m_storageController;
    AppController &m_app;
    // Before m_orchestrator: it is copied from the config that m_orchestrator's
    // initialiser then moves from.
    QString m_scratchRoot;
    Registry m_registry;
    SelectionModel m_selection;
    SqliteRunSink m_sink;
    std::unique_ptr<PowerInhibitor> m_inhibitor;
    Orchestrator m_orchestrator;
    std::unique_ptr<RunQueueModel> m_queue;
    std::unique_ptr<LogModel> m_log;
    std::map<QString, std::unique_ptr<TestState>> m_state;
    QStringList m_previousSelection;
    QString m_activeTab;
    QVariantList m_preconditions;
    bool m_preconditionsBlocking = false;
    int m_probesPending = 0;
    QTimer m_ticker;
    QString m_stage, m_stageColor;
    QDateTime m_runStarted;
    QStringList m_plannedIds;
    QHash<QString, double> m_estimateByTest;
    double m_actualMs = 0, m_estimatedMs = 0;
    QStringList m_finishedIds;
    QString m_currentId;            // the test running now (for the live ETA)
    QElapsedTimer m_currentClock;   // since it started
    QString m_etaText;
    QStringList m_startErrors;
    QThreadPool m_probePool;  // this controller's probe tasks only
    bool m_smoke = false;
    bool m_smokeBatteryStarted = false;
    // The held interface: what arrived while held, applied on release.
    // (Sub-test changes aren't consumed by this controller, so none are buffered.)
    bool m_held = false;
    std::optional<ProgressUpdate> m_heldProgress;
    QVector<LogLine> m_heldLog;
    QString m_heldPart, m_heldTestName;
    int m_heldIndex = -1;
};

} // namespace sb
