// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/controllers/run_controller.h"

#include "app/controllers/app_controller.h"
#include "app/controllers/storage_controller.h"
#include "app/models/log_model.h"
#include "app/models/options_model.h"
#include "app/models/run_queue_model.h"
#include "core/power_inhibitor.h"
#include "storage/storage.h"
#include "tests/builtin_tests.h"

#include "synbench_version.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QThreadPool>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcRun, "synbench.run")

namespace sb {

namespace {

class AppProbeContext : public ProbeContext
{
public:
    AppProbeContext(SystemSnapshot s, PrivilegedOps &ops) : m_system(std::move(s)), m_ops(ops) {}
    const SystemSnapshot &system() const override { return m_system; }
    PrivilegedOps &privileged() const override { return m_ops; }

private:
    SystemSnapshot m_system;
    PrivilegedOps &m_ops;
};

QString preconditionState(Precondition::State s)
{
    switch (s) {
    case Precondition::State::Met: return QStringLiteral("met");
    case Precondition::State::NotMet: return QStringLiteral("notmet");
    case Precondition::State::WillApplyAtStart: return QStringLiteral("apply");
    }
    return {};
}

Orchestrator::Config withPrivileged(Orchestrator::Config c, AppController &app)
{
    // AP5-R2-8: the orchestrator always gets a privileged backend; without one
    // tests would get an Unavailable stand-in silently.
    c.privileged = &app.privileged();
    Q_ASSERT(c.privileged);
    return c;
}

Registry &populated(Registry &r)
{
    registerBuiltinTests(r);
    return r;
}

} // namespace

RunController::RunController(Storage &storage, StorageController &storageController, AppController &app,
                             Orchestrator::Config config, QObject *parent)
    : QObject(parent),
      m_storage(storage),
      m_storageController(storageController),
      m_app(app),
      m_scratchRoot(config.scratchRoot),
      m_selection(populated(m_registry)),
      m_sink([this] { return m_storage.dbPath(); }),
      m_inhibitor(createSystemPowerInhibitor()),
      m_orchestrator(m_registry, m_sink, *m_inhibitor, withPrivileged(std::move(config), app)),
      m_queue(std::make_unique<RunQueueModel>()),
      m_log(std::make_unique<LogModel>(5000))
{
    m_log->setNameResolver([this](const QString &id) { return testName(id); });
    for (const auto &t : m_registry.tests())
        m_state.emplace(t->info().id, std::make_unique<TestState>());

    connect(&m_selection, &SelectionModel::selectionChanged, this, &RunController::onSelectionChanged);
    connectOrchestrator();

    // A tick for the elapsed times, the ETA and the polled preconditions.
    m_ticker.setInterval(1000);
    connect(&m_ticker, &QTimer::timeout, this, [this] {
        m_queue->tick();
        refreshEta();
        updatePreconditions();
    });
    m_ticker.start();

    // The snapshot (and the power state) arrives after startup: probe again.
    connect(&m_app, &AppController::systemInfoChanged, this, [this] {
        if (m_app.systemInfoReady() && !m_smoke)
            refreshProbes();
    });
    connect(&m_app, &AppController::powerChanged, this, &RunController::updatePreconditions);
    // Start is offered only once probing and startup recovery are done.
    connect(this, &RunController::probingChanged, this, &RunController::startabilityChanged);
    connect(&m_storageController, &StorageController::lockedChanged, this, &RunController::startabilityChanged);
    // AP-5 #9a: the privileged helper dying mid-run stops the run (saved as
    // Stopped, D3); the Retry / Quit dialog (D1) then blocks the window.
    connect(&m_app, &AppController::authChanged, this, [this] {
        if (m_app.authStateName() == QLatin1StringView("failed") && running()) {
            qCWarning(lcRun) << "the privileged helper stopped during a run; stopping the run";
            m_orchestrator.stop();
        }
    });
}

RunController::~RunController()
{
    // Probe tasks hold `this`, the registry and the privileged ops; let them
    // finish before any of those go away (their results are dropped with the
    // object's queued events).
    QThreadPool::globalInstance()->waitForDone();
}

RunController::TestState *RunController::stateOf(const QString &id) const
{
    const auto it = m_state.find(id);
    return it == m_state.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------- orchestrator

void RunController::connectOrchestrator()
{
    qRegisterMetaType<RunRecord>();
    qRegisterMetaType<TestResult>();
    qRegisterMetaType<ProgressUpdate>();
    qRegisterMetaType<LogLine>();

    connect(&m_orchestrator, &Orchestrator::runStarted, this, [this](const RunRecord &, const QStringList &ids) {
        m_plannedIds = ids;
        m_finishedIds.clear();
        m_actualMs = m_estimatedMs = 0;
        m_runStarted = QDateTime::currentDateTime();
        m_estimateByTest.clear();
        QVector<RunQueueModel::Planned> plan;
        for (const QString &id : ids) {
            plan.append({id, testName(id), testColor(id)});
            m_estimateByTest.insert(id, double(m_registry.find(id)->estimate(valuesFor(id)).count()));
        }
        m_queue->setPlan(plan);
        m_log->clear();
        m_stage.clear();
        m_storageController.setLocked(true, tr("A run is in progress."));
        refreshEta();
        emit runChanged();
        emit runStarted();
    });
    connect(&m_orchestrator, &Orchestrator::stateChanged, this, [this] { updateRunning(); });
    connect(&m_orchestrator, &Orchestrator::testStarted, this, [this](int index, const QString &id) {
        m_queue->testStarted(index);
        m_stage = testName(id);
        m_stageColor = testColor(id);
        emit currentChanged();
    });
    connect(&m_orchestrator, &Orchestrator::progress, this, [this](const ProgressUpdate &u) {
        m_queue->progress(u);
        m_stage = QStringLiteral("%1 · %2").arg(testName(u.testId), u.stage);
        emit currentChanged();
    });
    connect(&m_orchestrator, &Orchestrator::logLine, this, [this](const LogLine &l) { m_log->append(l); });
    connect(&m_orchestrator, &Orchestrator::testFinished, this, [this](int index, const TestResult &r) {
        m_queue->testFinished(index, r);
        if (r.status != Status::NotRun) {
            m_finishedIds.append(r.testId);
            const double est = m_estimateByTest.value(r.testId);
            if (est > 0) {
                m_actualMs += double(r.durationMs);
                m_estimatedMs += est * 1000.0;
            }
        }
        refreshEta();
        emit runUpdated();
    });
    connect(&m_orchestrator, &Orchestrator::runFinished, this, [this](const RunRecord &run) {
        // A run abandoned after a hung test leaves the storage locked until
        // restart; a normal finish unlocks it.
        if (m_orchestrator.state() != Orchestrator::State::Abandoned)
            m_storageController.setLocked(false);
        else
            m_storageController.setLocked(true, tr("A test stopped responding. Restart SynBench first."));
        m_stage.clear();
        m_etaText.clear();
        emit currentChanged();
        emit etaChanged();
        emit runChanged();
        m_storageController.historyChanged();
        emit runFinished(run.uuid.toString(QUuid::WithoutBraces), toKey(run.status));
    });
    connect(&m_orchestrator, &Orchestrator::storageError, this,
            [](const QString &message) { qCWarning(lcRun) << "history write failed:" << message; });
}

void RunController::updateRunning()
{
    emit runningChanged();
    emit startabilityChanged();
}

bool RunController::running() const
{
    const auto s = m_orchestrator.state();
    return s == Orchestrator::State::Running || s == Orchestrator::State::Stopping;
}

QString RunController::runUuid() const
{
    return m_orchestrator.currentRun().uuid.toString(QUuid::WithoutBraces);
}

QString RunController::runTitle() const
{
    const int n = int(m_plannedIds.size());
    if (n == 1)
        return running() ? tr("Running %1").arg(testName(m_plannedIds.first())) : tr("Run of %1").arg(testName(m_plannedIds.first()));
    return running() ? tr("Running %n tests", nullptr, n) : tr("Run of %n tests", nullptr, n);
}

QString RunController::runSubtitle() const
{
    if (!m_runStarted.isValid())
        return {};
    QString s = tr("Started %1").arg(m_runStarted.toString(QStringLiteral("HH:mm")));
    if (!m_etaText.isEmpty())
        s += QStringLiteral(" · ") + m_etaText;
    return s;
}

QObject *RunController::queue()
{
    return m_queue.get();
}

QObject *RunController::log()
{
    return m_log.get();
}

void RunController::refreshEta()
{
    QString text;
    if (running() && !m_plannedIds.isEmpty()) {
        const bool alone = m_registry.find(m_plannedIds.first())
                           && m_registry.find(m_plannedIds.first())->info().exclusivity == Exclusivity::RunsAlone;
        if (alone) {
            text = tr("runs until the battery is empty");
        } else {
            double remaining = 0;
            for (const QString &id : std::as_const(m_plannedIds)) {
                if (!m_finishedIds.contains(id))
                    remaining += m_estimateByTest.value(id);
            }
            // Corrected by how long the finished tests really took vs their estimates.
            const double ratio = m_estimatedMs > 0 ? m_actualMs / m_estimatedMs : 1.0;
            remaining *= ratio;
            const qint64 secs = qint64(std::llround(remaining));
            text = secs >= 90 ? tr("about %1 min left").arg((secs + 59) / 60) : tr("about %1 s left").arg(std::max<qint64>(secs, 1));
        }
    }
    if (text != m_etaText) {
        m_etaText = text;
        emit etaChanged();
    }
}

QString RunController::testName(const QString &id) const
{
    const ITest *t = m_registry.find(id);
    return t ? t->info().name : id;
}

QString RunController::testColor(const QString &id) const
{
    const ITest *t = m_registry.find(id);
    return t ? t->info().colorToken : QString();
}

// ---------------------------------------------------------------- probing

std::unique_ptr<ProbeContext> RunController::makeProbeContext() const
{
    return std::make_unique<AppProbeContext>(m_app.snapshotForRun(), m_app.privileged());
}

OptionSchema RunController::effectiveSchema(const QString &id) const
{
    const ITest *t = m_registry.find(id);
    if (!t)
        return {};
    const auto ctx = makeProbeContext();
    return effectiveOptionSchema(*t, *ctx, stateOf(id)->availability);
}

void RunController::refreshProbes()
{
    for (const auto &t : m_registry.tests()) {
        const QString id = t->info().id;
        ++m_probesPending;
        auto snapshot = m_app.snapshotForRun();
        PrivilegedOps *ops = &m_app.privileged();
        ITest *test = t.get();
        QThreadPool::globalInstance()->start([this, id, test, snapshot, ops] {
            // Discovery runs off the UI thread, never on it (§2.7).
            AppProbeContext ctx(snapshot, *ops);
            const Availability availability = test->probe(ctx);
            const OptionSchema schema = effectiveOptionSchema(*test, ctx, availability);
            QMetaObject::invokeMethod(this, [this, id, availability, schema] { onProbeResult(id, availability, schema); });
        });
    }
    emit probingChanged();
}

void RunController::onProbeResult(const QString &id, const Availability &availability, const OptionSchema &schema)
{
    --m_probesPending;
    TestState &st = *stateOf(id);
    st.availability = availability;
    st.probed = true;
    if (!st.model) {
        st.model = std::make_unique<OptionsModel>(schema, &st.values);
        st.model->setSchema(schema);  // fills in the defaults
        connect(st.model.get(), &OptionsModel::valueEdited, this, [this] {
            emit estimateChanged();
            emit startabilityChanged();
            updatePreconditions();
        });
    } else {
        st.model->setSchema(schema);
    }
    m_selection.setAvailability(id, availability);
    emit estimateChanged();
    emit startabilityChanged();
    if (id == m_activeTab)
        emit activeTabChanged();  // the options model may have been created
    if (m_probesPending == 0)
        emit probingChanged();
    updatePreconditions();
}

OptionValues RunController::valuesFor(const QString &testId)
{
    TestState *st = stateOf(testId);
    if (!st)
        return {};
    // Fill gaps (a test never probed) from the schema's defaults.
    return st->model ? st->model->schema().withDefaults(st->values) : st->values;
}

void RunController::setOption(const QString &testId, const QString &key, const QVariant &value)
{
    TestState *st = stateOf(testId);
    if (st && st->model)
        st->model->setValue(key, value);
}

// ---------------------------------------------------------------- selection

void RunController::toggle(const QString &testId)
{
    m_selection.toggle(testId);
}

void RunController::selectAll()
{
    m_selection.selectAll();
}

void RunController::clearSelection()
{
    m_selection.clear();
}

void RunController::onSelectionChanged()
{
    const QStringList now = m_selection.selectedIds();
    // The newly ticked test becomes the active tab (as in the mockup).
    QString newest;
    for (const QString &id : now) {
        if (!m_previousSelection.contains(id))
            newest = id;
    }
    m_previousSelection = now;
    if (!newest.isEmpty())
        m_activeTab = newest;
    else if (!now.contains(m_activeTab))
        m_activeTab = now.value(0);
    emit selectionChanged();
    emit activeTabChanged();
    emit estimateChanged();
    emit startabilityChanged();
    updatePreconditions();
}

QVariantList RunController::hardwareCheck() const
{
    QVariantList out;
    for (const QString &id : m_registry.ids()) {
        const TestState *st = stateOf(id);
        if (!st)
            continue;
        const Availability &a = st->availability;
        QString state = QStringLiteral("good"), detail;
        if (!st->probed) {
            state = QStringLiteral("partial");
            detail = tr("Checking…");
        } else if (a.state == Availability::State::Unavailable) {
            state = QStringLiteral("na");
            detail = a.reason;
        } else {
            if (a.state == Availability::State::Partial)
                state = QStringLiteral("partial");
            QStringList caps;
            for (const CapabilityItem &c : a.capabilities)
                caps << c.label;
            detail = caps.isEmpty() ? a.reason : caps.join(QStringLiteral(", "));
            if (a.state == Availability::State::Partial && !a.reason.isEmpty() && !detail.contains(a.reason))
                detail += detail.isEmpty() ? a.reason : QStringLiteral(" (%1)").arg(a.reason);
        }
        out.append(QVariantMap{{QStringLiteral("id"), id},
                               {QStringLiteral("name"), testName(id)},
                               {QStringLiteral("colorToken"), testColor(id)},
                               {QStringLiteral("state"), state},
                               {QStringLiteral("detail"), detail},
                               {QStringLiteral("hint"), a.hint ? a.hint->text : QString()}});
    }
    return out;
}

QVariantList RunController::tabs() const
{
    QVariantList out;
    for (const QString &id : m_selection.selectedIds()) {
        out.append(QVariantMap{{QStringLiteral("id"), id},
                               {QStringLiteral("name"), testName(id)},
                               {QStringLiteral("colorToken"), testColor(id)}});
    }
    return out;
}

void RunController::setActiveTab(const QString &id)
{
    if (id == m_activeTab || !m_selection.selectedIds().contains(id))
        return;
    m_activeTab = id;
    emit activeTabChanged();
    updatePreconditions();
}

QObject *RunController::options()
{
    TestState *st = stateOf(m_activeTab);
    return st ? st->model.get() : nullptr;
}

// ---------------------------------------------------------------- estimate and start

QString RunController::estimateText() const
{
    if (selectedCount() == 0)
        return {};
    if (runsAloneSelected())
        return tr("runs on its own until the battery is empty");
    qint64 secs = 0;
    for (const QString &id : m_selection.selectedIds()) {
        const ITest *t = m_registry.find(id);
        const TestState *st = stateOf(id);
        if (t) {
            OptionValues v = st ? st->values : OptionValues{};
            if (st && st->model)
                v = st->model->schema().withDefaults(st->values);
            secs += t->estimate(v).count();
        }
    }
    return secs >= 90 ? tr("estimated %1 min").arg((secs + 59) / 60) : tr("estimated %1 s").arg(std::max<qint64>(secs, 1));
}

QString RunController::estimateOf(const QString &testId)
{
    const ITest *t = m_registry.find(testId);
    if (!t)
        return {};
    const qint64 secs = t->estimate(valuesFor(testId)).count();
    return secs >= 90 ? tr("~%1 min").arg((secs + 59) / 60) : tr("~%1 s").arg(std::max<qint64>(secs, 1));
}

QString RunController::runButtonText() const
{
    return runsAloneSelected() ? tr("Start battery run") : tr("Run selected tests");
}

QStringList RunController::validationErrors() const
{
    QStringList errors;
    for (const QString &id : m_selection.selectedIds()) {
        const ITest *t = m_registry.find(id);
        const TestState *st = stateOf(id);
        if (!t || !st || !st->probed)
            continue;
        const OptionValues v = st->model ? st->model->schema().withDefaults(st->values) : st->values;
        const OptionSchema schema = st->model ? st->model->schema() : OptionSchema{};
        for (const ValidationError &e : validateOptions(schema, v).errors)
            errors.append(QStringLiteral("%1: %2").arg(t->info().name, e.message));
        for (const ValidationError &e : t->validate(v).errors)
            errors.append(QStringLiteral("%1: %2").arg(t->info().name, e.message));
    }
    return errors;
}

QStringList RunController::problems() const
{
    QStringList out;
    if (m_app.authStateName() != QLatin1StringView("ready"))
        out.append(tr("Waiting for administrator access."));
    if (!m_storage.isOpen())
        out.append(tr("The history isn't open."));
    if (needsRestart())
        out.append(tr("A test stopped responding. Restart SynBench to run tests again."));
    else if (running())
        out.append(tr("A run is already in progress."));
    out += validationErrors();
    for (const QVariant &p : m_preconditions) {
        const QVariantMap m = p.toMap();
        if (m.value(QStringLiteral("state")).toString() == QLatin1StringView("notmet"))
            out.append(QStringLiteral("%1: %2").arg(m.value(QStringLiteral("test")).toString(), m.value(QStringLiteral("label")).toString()));
    }
    return out;
}

bool RunController::canStart() const
{
    return selectedCount() > 0 && problems().isEmpty() && !probing() && !m_storageController.lockedBy(QStringLiteral("recovery"));
}

void RunController::updatePreconditions()
{
    QVariantList list;
    bool blocking = false;
    const auto ctx = makeProbeContext();
    for (const QString &id : m_selection.selectedIds()) {
        const ITest *t = m_registry.find(id);
        const TestState *st = stateOf(id);
        if (!t || !st || !st->probed)
            continue;
        const OptionValues v = st->model ? st->model->schema().withDefaults(st->values) : st->values;
        for (const Precondition &p : t->preconditions(v, *ctx)) {
            list.append(QVariantMap{{QStringLiteral("test"), t->info().name},
                                    {QStringLiteral("testId"), id},
                                    {QStringLiteral("id"), p.id},
                                    {QStringLiteral("label"), p.label},
                                    {QStringLiteral("state"), preconditionState(p.state)},
                                    {QStringLiteral("detail"), p.detail}});
            blocking = blocking || p.state == Precondition::State::NotMet;
        }
    }
    if (list != m_preconditions) {
        m_preconditions = list;
        emit preconditionsChanged();
        emit startabilityChanged();
    }
    m_preconditionsBlocking = blocking;
}

QVariantList RunController::preconditions() const
{
    QVariantList forTab;
    for (const QVariant &p : m_preconditions) {
        if (p.toMap().value(QStringLiteral("testId")).toString() == m_activeTab)
            forTab.append(p);
    }
    return forTab;
}

bool RunController::start()
{
    m_startErrors.clear();
    // Recovery treats every 'running' row as a leftover: never start (and so
    // write one) until it has finished.
    if (m_storageController.lockedBy(QStringLiteral("recovery"))) {
        m_startErrors = {tr("SynBench is still checking earlier runs. Try again in a moment.")};
        emit startRefused(m_startErrors);
        return false;
    }
    Plan plan;
    for (const QString &id : m_selection.selectedIds())
        plan.append({id, valuesFor(id)});
    RunMeta meta;
    meta.appVersion = QStringLiteral(SYNBENCH_DISPLAY_VERSION);
    meta.appBuild = QStringLiteral(SYNBENCH_BUILD_ID);
    meta.system = m_app.snapshotForRun();
    const auto result = m_orchestrator.start(plan, meta);
    if (!result.ok) {
        m_startErrors = result.errors;
        emit startRefused(result.errors);
        return false;
    }
    m_app.setSection(QStringLiteral("progress"));
    return true;
}

void RunController::stop()
{
    m_orchestrator.stop();
}

// ---------------------------------------------------------------- smoke

void RunController::runSmoke()
{
    m_smoke = true;
    // Probe, then run everything once the probes are in.
    const auto begin = [this] {
        if (probing() || running() || m_smokeBatteryStarted || hasRun() || m_storageController.lockedBy(QStringLiteral("recovery")))
            return;
        m_selection.selectAll();
        if (!start()) {
            qCCritical(lcRun) << "smoke: couldn't start:" << m_startErrors;
            QCoreApplication::exit(1);
        }
    };
    connect(this, &RunController::probingChanged, this, begin);
    connect(&m_storageController, &StorageController::lockedChanged, this, begin);
    connect(this, &RunController::runFinished, this, [this](const QString &, const QString &status) {
        const RunRecord &run = currentRun();
        bool ok = status == QLatin1StringView("completed");
        for (const TestResult &t : run.tests) {
            if (t.status != Status::Ok) {
                qCCritical(lcRun) << "smoke:" << t.testId << "ended" << toKey(t.status) << t.statusReason;
                ok = false;
            }
        }
        if (!ok) {
            QCoreApplication::exit(1);
            return;
        }
        if (!m_smokeBatteryStarted) {
            // The exclusive stub, with its desktop override (D15).
            m_smokeBatteryStarted = true;
            m_selection.clear();
            m_selection.setSelected(QStringLiteral("battery"), true);
            setOption(QStringLiteral("battery"), QStringLiteral("override_preconditions"), true);
            if (!start()) {
                qCCritical(lcRun) << "smoke: couldn't start Battery:" << m_startErrors;
                QCoreApplication::exit(1);
            }
            return;
        }
        qCInfo(lcRun) << "smoke: all stubs ran; the history has" << m_storageController.runCount() << "runs";
        QCoreApplication::exit(m_storageController.runCount() == 2 ? 0 : 1);
    });
    refreshProbes();
}

} // namespace sb
