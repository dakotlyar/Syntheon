// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/controllers/results_controller.h"

#include "app/controllers/run_controller.h"
#include "app/controllers/storage_controller.h"
#include "app/models/detail_blocks.h"
#include "app/models/log_filter_model.h"
#include "app/models/log_model.h"
#include "core/units.h"
#include "storage/run_repository.h"
#include "storage/storage.h"

#include <QLocale>
#include <QQmlEngine>

namespace sb {

namespace {

const QString kBattery = QStringLiteral("battery");

QString profileName(const QVariantMap &options)
{
    const QString p = options.value(QStringLiteral("profile")).toString();
    return p.isEmpty() ? QString() : p.left(1).toUpper() + p.mid(1);
}

} // namespace

ResultsController::ResultsController(RunController &run, Storage &storage, StorageController &storageController,
                                     QObject *parent)
    : QObject(parent), m_runController(run), m_storage(storage), m_storageController(storageController),
      m_logSource(std::make_unique<LogModel>(1000000)), m_logFilter(std::make_unique<LogFilterModel>())
{
    // logModel() hands this object to QML from an invokable; without this QML
    // would take ownership of it and delete it.
    QQmlEngine::setObjectOwnership(m_logFilter.get(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(m_logSource.get(), QQmlEngine::CppOwnership);
    m_logSource->setNameResolver([this](const QString &id) { return m_runController.testName(id); });
    m_logFilter->setSourceModel(m_logSource.get());
    // "View results so far" follows the live run as tests finish.
    connect(&m_runController, &RunController::runUpdated, this, [this] {
        if (m_live)
            setRun(m_runController.currentRun(), true);
    });
    connect(&m_runController, &RunController::runFinished, this, [this](const QString &uuid) {
        if (m_live || !m_run)  // it was showing this run (or nothing): switch to the stored copy
            openRun(uuid);
    });
    connect(&m_storageController, &StorageController::historyChanged, this, [this] {
        // A cleared history or a switched location: don't keep showing a run that is gone.
        if (m_run && !m_live && !openRun(uuid())) {
            m_run.reset();
            m_rows.clear();
            m_tabs.clear();
            m_detailBlocks.clear();
            emit runChanged();
            emit detailChanged();
        }
    });
}

ResultsController::~ResultsController() = default;

// ---------------------------------------------------------------- loading

bool ResultsController::openRun(const QString &uuid)
{
    if (!m_storage.isOpen())
        return false;
    RunRepository repo(m_storage.connection());
    QUuid id = QUuid::fromString(uuid);
    if (id.isNull()) {
        const auto latest = repo.latestRun();
        if (!latest)
            return false;
        id = *latest;
    }
    // The run in progress isn't final in the database yet; show the live copy.
    if (m_runController.running() && m_runController.currentRun().uuid == id) {
        setRun(m_runController.currentRun(), true);
        return true;
    }
    const auto run = repo.load(id);
    if (!run)
        return false;
    setRun(*run, false);
    return true;
}

void ResultsController::openCurrent()
{
    if (m_runController.hasRun()) {
        setRun(m_runController.currentRun(), m_runController.running());
        return;
    }
    openRun({});
}

void ResultsController::setRun(RunRecord run, bool live)
{
    const QString previousDetail = m_detailId;
    const bool sameRun = m_run && m_run->uuid == run.uuid;
    m_run = std::move(run);
    m_live = live;
    if (!sameRun)
        closeDetail();
    m_logSource->clear();
    rebuild();
    emit runChanged();
    // Keep the Detail page on the same test while a live run updates.
    if (sameRun && !previousDetail.isEmpty())
        selectDetail(previousDetail);
    else {
        m_detailId.clear();
        m_detailBlocks.clear();
        emit detailChanged();
    }
    emit logChanged();
}

// ---------------------------------------------------------------- run summary

QString ResultsController::formatDate(const QDateTime &t)
{
    return QLocale(QLocale::English).toString(t.toLocalTime(), QStringLiteral("d MMM yyyy, HH:mm"));
}

QString ResultsController::statusLabelFor(RunStatus s)
{
    switch (s) {
    case RunStatus::Running: return QStringLiteral("Running");
    case RunStatus::Completed: return {};
    case RunStatus::Stopped: return QStringLiteral("Stopped");
    case RunStatus::Interrupted: return QStringLiteral("Interrupted");
    case RunStatus::Recovered: return QStringLiteral("Recovered");
    }
    return {};
}

QString ResultsController::uuid() const
{
    return m_run ? m_run->uuid.toString(QUuid::WithoutBraces) : QString();
}

QString ResultsController::title() const
{
    return m_live ? tr("Results so far") : tr("Results");
}

QString ResultsController::subtitle() const
{
    if (!m_run)
        return tr("No saved runs yet.");
    const int n = int(m_run->tests.size());
    const QString machine = m_run->system.machineLabel;
    return QStringLiteral("%1 %2 · %3 · %4")
        .arg(m_live ? tr("Run started") : tr("Run of"), formatDate(m_run->startedAt),
             machine.isEmpty() ? tr("this machine") : machine, n == 1 ? tr("1 test") : tr("%1 tests").arg(n));
}

QString ResultsController::statusKey() const
{
    return m_run ? toKey(m_run->status) : QString();
}

QString ResultsController::statusLabel() const
{
    return m_run ? statusLabelFor(m_run->status) : QString();
}

std::optional<double> ResultsController::overallValue() const
{
    if (!m_run)
        return std::nullopt;
    if (m_run->overall)
        return m_run->overall;
    // A run in progress hasn't had its overall written yet.
    return computeOverall(m_run->tests, [this](const QString &id) {
        const ITest *t = m_runController.registry().find(id);
        return t && t->info().countsTowardOverall;
    });
}

QString ResultsController::overallText() const
{
    const auto v = overallValue();
    return v ? Units::format(*v, QStringLiteral("points"), false) : QStringLiteral("—");
}

bool ResultsController::overallPartial() const
{
    // N2: a Stopped run's overall is only what finished.
    return m_run && (m_run->status == RunStatus::Stopped || m_live) && overallValue().has_value();
}

double ResultsController::scoredFraction() const
{
    if (!m_run || m_run->tests.isEmpty())
        return 0;
    int scored = 0;
    for (const TestResult &t : m_run->tests)
        scored += t.score ? 1 : 0;
    return double(scored) / double(m_run->tests.size());
}

QString ResultsController::overallNote() const
{
    if (!m_run)
        return {};
    if (m_live)
        return tr("Partial: sum of the scores so far");
    if (m_run->status == RunStatus::Stopped)
        return tr("Partial: the run was stopped");
    if (m_run->status == RunStatus::Interrupted)
        return tr("Partial: the run was interrupted");
    int scored = 0;
    for (const TestResult &t : m_run->tests)
        scored += t.score ? 1 : 0;
    return scored == 0 ? tr("Nothing produced a score") : tr("Sum of available test scores");
}

QString ResultsController::scoreText(const TestResult &t) const
{
    if (t.score)
        return Units::format(*t.score, QStringLiteral("points"), false);
    switch (t.status) {
    case Status::NotAvailable: return QStringLiteral("N/A");
    case Status::Error: return tr("Error");
    case Status::Cancelled: return tr("Stopped");
    case Status::TimedOut: return tr("Timed out");
    case Status::Interrupted: return tr("Interrupted");
    case Status::NotRun: return tr("Not run");
    default: return QStringLiteral("—");
    }
}

QString ResultsController::rawSummary(const TestResult &t) const
{
    if (t.status != Status::Ok && t.status != Status::Partial)
        return t.statusReason;
    const ITest *test = m_runController.registry().find(t.testId);
    QStringList parts;
    const DetailLayout layout = test ? test->detail(t) : genericDetailLayout(t);
    for (const QString &key : layout.headlineMetrics) {
        const Metric *m = t.metric(key);
        if (!m)
            continue;
        parts.append(QStringLiteral("%1 %2").arg(m->label, formatMetric(t, key)));
        if (parts.size() == 3)
            break;
    }
    return parts.join(QStringLiteral(" · "));
}

void ResultsController::rebuild()
{
    m_rows.clear();
    m_tabs.clear();
    m_batteryRun.reset();
    if (!m_run)
        return;
    bool hasBattery = false;
    for (const TestResult &t : std::as_const(m_run->tests)) {
        const QString token = m_runController.testColor(t.testId);
        const bool na = t.status == Status::NotAvailable;
        m_rows.append(QVariantMap{{QStringLiteral("testId"), t.testId},
                                  {QStringLiteral("name"), m_runController.testName(t.testId)},
                                  {QStringLiteral("colorToken"), token},
                                  {QStringLiteral("score"), scoreText(t)},
                                  {QStringLiteral("raw"), rawSummary(t)},
                                  {QStringLiteral("na"), na || !t.score},
                                  {QStringLiteral("status"), toKey(t.status)}});
        m_tabs.append(QVariantMap{{QStringLiteral("id"), t.testId},
                                  {QStringLiteral("name"), m_runController.testName(t.testId)},
                                  {QStringLiteral("colorToken"), token},
                                  {QStringLiteral("na"), na},
                                  {QStringLiteral("separate"), false}});
        hasBattery = hasBattery || t.testId == kBattery;
    }
    // D4 + N4: a "Battery" tab with the newest Battery run on or before this
    // run's date, labelled as coming from a separate run.
    if (!hasBattery && m_storage.isOpen()) {
        RunRepository repo(m_storage.connection());
        if (const auto id = repo.latestBatteryRun(m_run->startedAt)) {
            if (auto battery = repo.load(*id)) {
                m_batteryRun = std::move(battery);
                m_tabs.append(QVariantMap{{QStringLiteral("id"), kBattery},
                                          {QStringLiteral("name"), m_runController.testName(kBattery)},
                                          {QStringLiteral("colorToken"), m_runController.testColor(kBattery)},
                                          {QStringLiteral("na"), false},
                                          {QStringLiteral("separate"), true}});
            }
        }
    }
}

// ---------------------------------------------------------------- detail

void ResultsController::showDetail(const QString &testId)
{
    selectDetail(testId);
    if (!m_detailOpen) {
        m_detailOpen = true;
        emit detailOpenChanged();
    }
}

void ResultsController::closeDetail()
{
    if (m_detailOpen) {
        m_detailOpen = false;
        emit detailOpenChanged();
    }
}

void ResultsController::selectDetail(const QString &testId)
{
    m_detailId = testId;
    buildDetail();
    emit detailChanged();
}

void ResultsController::buildDetail()
{
    m_detailBlocks.clear();
    m_detailTitle.clear();
    m_detailSubtitle.clear();
    m_detailNote.clear();
    m_detailColor.clear();
    if (!m_run || m_detailId.isEmpty())
        return;

    const TestResult *result = nullptr;
    const RunRecord *source = &*m_run;
    for (const TestResult &t : std::as_const(m_run->tests)) {
        if (t.testId == m_detailId)
            result = &t;
    }
    if (!result && m_detailId == kBattery && m_batteryRun) {
        source = &*m_batteryRun;
        for (const TestResult &t : std::as_const(m_batteryRun->tests)) {
            if (t.testId == kBattery)
                result = &t;
        }
    }
    if (!result)
        return;

    const ITest *test = m_runController.registry().find(m_detailId);
    const DetailLayout layout = test ? test->detail(*result) : genericDetailLayout(*result);
    m_detailColor = m_runController.testColor(m_detailId);
    m_detailTitle = m_runController.testName(m_detailId);
    m_detailSubtitle = layout.subtitle;
    m_detailBlocks = buildDetailBlocks(layout, *result, m_detailColor);
    if (source != &*m_run) {
        const QString profile = profileName(result->options);
        m_detailNote = tr("From a separate run · %1%2")
                           .arg(profile.isEmpty() ? QString() : profile + tr(" profile") + QStringLiteral(" · "),
                                formatDate(source->startedAt));
    }
}

// ---------------------------------------------------------------- the stored log (D16)

QObject *ResultsController::logModel()
{
    if (!m_run)
        return m_logFilter.get();
    if (m_live) {
        // The live ring buffer is what "View log" shows during a run.
        m_logSource->setLines({});
        const auto *live = qobject_cast<LogModel *>(m_runController.log());
        for (int i = 0; live && i < live->rowCount(); ++i) {
            const QModelIndex idx = live->index(i);
            LogLine l;
            l.testId = live->data(idx, LogModel::TestIdRole).toString();
            l.message = live->data(idx, LogModel::MessageRole).toString();
            l.level = logLevelFromKey(live->data(idx, LogModel::LevelRole).toString()).value_or(LogLevel::Info);
            l.timestamp = QDateTime::currentDateTime();
            m_logSource->append(l);
        }
        return m_logFilter.get();
    }
    RunRepository repo(m_storage.connection());
    const auto id = repo.runId(m_run->uuid);
    m_logSource->setLines(id ? repo.loadLog(*id) : QVector<LogLine>{});
    return m_logFilter.get();
}

QVariantList ResultsController::logTests() const
{
    QVariantList out;
    if (!m_run)
        return out;
    for (const TestResult &t : std::as_const(m_run->tests)) {
        out.append(QVariantMap{{QStringLiteral("value"), t.testId}, {QStringLiteral("label"), m_runController.testName(t.testId)}});
    }
    return out;
}

} // namespace sb
