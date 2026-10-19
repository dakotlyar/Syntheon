// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/controllers/compare_controller.h"

#include "app/controllers/results_controller.h"
#include "app/controllers/run_controller.h"
#include "app/controllers/storage_controller.h"
#include "app/models/detail_blocks.h"
#include "core/units.h"
#include "storage/run_repository.h"
#include "storage/storage.h"

#include <QLocale>

#include <algorithm>
#include <cmath>

namespace sb {

namespace {

QString scoreCell(const TestResult *t)
{
    if (!t)
        return QStringLiteral("—");
    if (t->score)
        return Units::format(*t->score, QStringLiteral("points"), false);
    switch (t->status) {
    case Status::NotAvailable: return QStringLiteral("N/A");
    case Status::Error: return QStringLiteral("Error");
    case Status::Cancelled: return QStringLiteral("Stopped");
    case Status::TimedOut: return QStringLiteral("Timed out");
    case Status::Interrupted: return QStringLiteral("Interrupted");
    case Status::NotRun: return QStringLiteral("Not run");
    default: return QStringLiteral("—");
    }
}

QString dir(double pct, bool lowerIsBetter, bool neutral)
{
    if (neutral || std::fabs(pct) < 0.5)
        return QStringLiteral("none");
    const bool up = pct > 0;
    return up != lowerIsBetter ? QStringLiteral("good") : QStringLiteral("bad");
}

} // namespace

CompareController::CompareController(RunController &run, Storage &storage, StorageController &storageController, QObject *parent)
    : QObject(parent), m_run(run), m_storage(storage), m_storageController(storageController)
{
    // The compared runs disappear with the history.
    connect(&m_storageController, &StorageController::historyChanged, this, [this] {
        if (m_runs.isEmpty() || !m_storage.isOpen())
            return;
        RunRepository repo(m_storage.connection());
        QStringList alive;
        for (const RunRecord &r : std::as_const(m_runs)) {
            if (repo.runId(r.uuid))
                alive.append(r.uuid.toString(QUuid::WithoutBraces));
        }
        if (alive.size() != m_runs.size())
            setRuns(alive);
    });
}

QString CompareController::changeText(double pct, bool lowerIsBetter)
{
    if (!std::isfinite(pct))
        return {};  // NaN / inf: nothing sensible to say
    // llround of a huge double is undefined: cap what's displayed.
    constexpr double kCap = 99999;
    const long long r = std::llround(std::clamp(pct, -kCap, kCap));
    if (r == 0)
        return QStringLiteral("0%");
    QString text = QStringLiteral("%1%2%").arg(r > 0 ? QStringLiteral("+") : QString()).arg(r);
    if (lowerIsBetter && r > 0)
        text += QStringLiteral(" (worse)");
    return text;
}

QString CompareController::baselineUuid() const
{
    return m_runs.isEmpty() ? QString() : m_runs[m_baseline].uuid.toString(QUuid::WithoutBraces);
}

QString CompareController::subtitle() const
{
    if (!valid())
        return tr("Pick two or more runs in History to compare them.");
    return tr("Comparing %1 runs. Changes are measured against the baseline, the newest run unless you pick another.")
        .arg(m_runs.size());
}

QString CompareController::rawTestName() const
{
    return m_run.testName(m_rawTestId);
}

bool CompareController::setRuns(const QStringList &uuids)
{
    m_runs.clear();
    m_baseline = 0;
    if (m_storage.isOpen()) {
        RunRepository repo(m_storage.connection());
        for (const QString &id : uuids) {
            if (auto run = repo.load(QUuid::fromString(id)))
                m_runs.append(*run);
        }
    }
    std::sort(m_runs.begin(), m_runs.end(), [](const RunRecord &a, const RunRecord &b) { return a.startedAt > b.startedAt; });
    // Default baseline: the newest run (index 0).
    m_rawTestId.clear();
    rebuild();
    emit changed();
    emit rawChanged();
    return valid();
}

void CompareController::setBaseline(const QString &uuid)
{
    for (int i = 0; i < m_runs.size(); ++i) {
        if (m_runs[i].uuid.toString(QUuid::WithoutBraces) == uuid && i != m_baseline) {
            m_baseline = i;
            rebuild();
            emit changed();
            emit rawChanged();
            return;
        }
    }
}

void CompareController::setRawTest(const QString &testId)
{
    if (testId == m_rawTestId)
        return;
    m_rawTestId = testId;
    rebuildRaw();
    emit rawChanged();
}

const TestResult *CompareController::resultOf(const RunRecord &run, const QString &testId) const
{
    for (const TestResult &t : run.tests) {
        if (t.testId == testId)
            return &t;
    }
    return nullptr;
}

void CompareController::rebuild()
{
    m_runColumns.clear();
    m_scoreRows.clear();
    m_rawTests.clear();
    m_rawRows.clear();
    if (m_runs.isEmpty())
        return;

    for (int i = 0; i < m_runs.size(); ++i) {
        const RunRecord &r = m_runs[i];
        m_runColumns.append(QVariantMap{
            {QStringLiteral("uuid"), r.uuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("date"), ResultsController::formatDate(r.startedAt)},
            {QStringLiteral("machine"), r.system.machineLabel},
            // Two short header lines that tell same-day runs apart: the day, then
            // the time and the machine's host name ("13:38 · Knowhere").
            {QStringLiteral("day"), QLocale(QLocale::English).toString(r.startedAt.toLocalTime(), QStringLiteral("d MMM yyyy"))},
            {QStringLiteral("timeAndHost"),
             QLocale(QLocale::English).toString(r.startedAt.toLocalTime(), QStringLiteral("HH:mm"))
                 + (r.system.machineLabel.isEmpty() ? QString() : QStringLiteral(" · ") + r.system.machineLabel.section(QStringLiteral(" · "), 0, 0))},
            {QStringLiteral("status"), ResultsController::statusLabelFor(r.status)},
            {QStringLiteral("baseline"), i == m_baseline},
        });
    }

    // Tests in registry order, then any a removed test left behind.
    QStringList ids = m_run.registry().ids();
    for (const RunRecord &r : std::as_const(m_runs)) {
        for (const TestResult &t : r.tests) {
            if (!ids.contains(t.testId))
                ids.append(t.testId);
        }
    }

    const RunRecord &base = m_runs[m_baseline];
    for (const QString &id : std::as_const(ids)) {
        bool anywhere = false;
        for (const RunRecord &r : std::as_const(m_runs))
            anywhere = anywhere || resultOf(r, id);
        if (!anywhere)
            continue;

        const TestResult *b = resultOf(base, id);
        QVariantList cells, bars;
        double maxScore = 0;
        for (const RunRecord &r : std::as_const(m_runs)) {
            if (const TestResult *t = resultOf(r, id); t && t->score)
                maxScore = std::max(maxScore, *t->score);
        }
        bool rowChanged = false;
        for (int i = 0; i < m_runs.size(); ++i) {
            const TestResult *t = resultOf(m_runs[i], id);
            QVariantMap cell{{QStringLiteral("text"), scoreCell(t)},
                             {QStringLiteral("na"), !t || !t->score},
                             {QStringLiteral("changeText"), QString()},
                             {QStringLiteral("direction"), QStringLiteral("none")},
                             {QStringLiteral("scoringChanged"), false}};
            // D6: another test or formula version than the baseline's.
            if (t && b && (t->testVersion != b->testVersion || t->formulaVersion != b->formulaVersion)) {
                cell.insert(QStringLiteral("scoringChanged"), true);
                rowChanged = true;
            }
            if (i != m_baseline && t && b && t->score && b->score && *b->score != 0) {
                const double pct = (*t->score - *b->score) / *b->score * 100.0;
                if (std::isfinite(pct)) {
                    cell.insert(QStringLiteral("changeText"), changeText(pct, false));
                    cell.insert(QStringLiteral("direction"), dir(pct, false, false));
                }
            }
            cells.append(cell);
            bars.append(t && t->score && maxScore > 0 ? *t->score / maxScore : 0.0);
        }
        m_scoreRows.append(QVariantMap{{QStringLiteral("testId"), id},
                                       {QStringLiteral("name"), m_run.testName(id)},
                                       {QStringLiteral("colorToken"), m_run.testColor(id)},
                                       {QStringLiteral("cells"), cells},
                                       {QStringLiteral("bars"), bars},
                                       {QStringLiteral("scoringChanged"), rowChanged},
                                       {QStringLiteral("overall"), false}});
    }

    // The overall row: partial marker for a Stopped run (N2).
    {
        QVariantList cells, bars;
        double maxOverall = 0;
        for (const RunRecord &r : std::as_const(m_runs)) {
            if (r.overall)
                maxOverall = std::max(maxOverall, *r.overall);
        }
        const RunRecord &baseRun = m_runs[m_baseline];
        for (int i = 0; i < m_runs.size(); ++i) {
            const RunRecord &r = m_runs[i];
            QVariantMap cell{{QStringLiteral("text"), r.overall ? Units::format(*r.overall, QStringLiteral("points"), false) : QStringLiteral("—")},
                             {QStringLiteral("na"), !r.overall},
                             {QStringLiteral("changeText"), QString()},
                             {QStringLiteral("direction"), QStringLiteral("none")},
                             {QStringLiteral("scoringChanged"), false},
                             {QStringLiteral("partial"), isPartialRun(r.status) && r.overall.has_value()}};
            // A partial overall isn't comparable to a full one: no change % at
            // all (not a big red -63%), just the marker.
            const bool partialPair = isPartialRun(r.status) || isPartialRun(baseRun.status);
            if (i != m_baseline && r.overall && baseRun.overall && *baseRun.overall != 0 && !partialPair) {
                const double pct = (*r.overall - *baseRun.overall) / *baseRun.overall * 100.0;
                if (std::isfinite(pct)) {
                    cell.insert(QStringLiteral("changeText"), changeText(pct, false));
                    cell.insert(QStringLiteral("direction"), dir(pct, false, false));
                }
            }
            cells.append(cell);
            bars.append(r.overall && maxOverall > 0 ? *r.overall / maxOverall : 0.0);
        }
        bool any = false;
        for (const RunRecord &r : std::as_const(m_runs))
            any = any || r.overall.has_value();
        if (any) {
            m_scoreRows.append(QVariantMap{{QStringLiteral("testId"), QStringLiteral("overall")},
                                           {QStringLiteral("name"), tr("Overall")},
                                           {QStringLiteral("colorToken"), QString()},
                                           {QStringLiteral("cells"), cells},
                                           {QStringLiteral("bars"), bars},
                                           {QStringLiteral("scoringChanged"), false},
                                           {QStringLiteral("overall"), true}});
        }
    }

    // Tests whose raw metrics can be compared (present with metrics in >= 1 run).
    for (const QString &id : std::as_const(ids)) {
        bool has = false;
        for (const RunRecord &r : std::as_const(m_runs)) {
            const TestResult *t = resultOf(r, id);
            has = has || (t && !t->metrics.isEmpty());
        }
        if (has)
            m_rawTests.append(QVariantMap{{QStringLiteral("value"), id}, {QStringLiteral("label"), m_run.testName(id)}});
    }
    if (m_rawTestId.isEmpty() || !std::any_of(m_rawTests.cbegin(), m_rawTests.cend(), [this](const QVariant &v) {
            return v.toMap().value(QStringLiteral("value")).toString() == m_rawTestId;
        })) {
        // Prefer a test every run has; else the first with metrics.
        m_rawTestId = m_rawTests.isEmpty() ? QString() : m_rawTests.first().toMap().value(QStringLiteral("value")).toString();
        for (const QVariant &v : std::as_const(m_rawTests)) {
            const QString id = v.toMap().value(QStringLiteral("value")).toString();
            bool all = true;
            for (const RunRecord &r : std::as_const(m_runs))
                all = all && resultOf(r, id) && !resultOf(r, id)->metrics.isEmpty();
            if (all) {
                m_rawTestId = id;
                break;
            }
        }
    }
    rebuildRaw();
}

void CompareController::rebuildRaw()
{
    m_rawRows.clear();
    if (m_runs.isEmpty() || m_rawTestId.isEmpty())
        return;
    // Metric keys in order of first appearance, baseline run first.
    QStringList keys;
    QVector<int> order{m_baseline};
    for (int i = 0; i < m_runs.size(); ++i) {
        if (i != m_baseline)
            order.append(i);
    }
    QHash<QString, const Metric *> firstSeen;
    for (int i : order) {
        const TestResult *t = resultOf(m_runs[i], m_rawTestId);
        if (!t)
            continue;
        for (const Metric &m : t->metrics) {
            if (!keys.contains(m.key)) {
                keys.append(m.key);
                firstSeen.insert(m.key, &m);
            }
        }
    }
    const TestResult *base = resultOf(m_runs[m_baseline], m_rawTestId);
    for (const QString &key : std::as_const(keys)) {
        const Metric *proto = firstSeen.value(key);
        const bool lower = proto->direction == Direction::LowerIsBetter;
        const bool neutral = proto->direction == Direction::Neutral;
        const Metric *bm = base ? base->metric(key) : nullptr;
        QVariantList cells;
        for (int i = 0; i < m_runs.size(); ++i) {
            const TestResult *t = resultOf(m_runs[i], m_rawTestId);
            const Metric *m = t ? t->metric(key) : nullptr;
            QVariantMap cell{{QStringLiteral("text"), t && m ? formatMetric(*t, key) : QStringLiteral("—")},
                             {QStringLiteral("changeText"), QString()},
                             {QStringLiteral("direction"), QStringLiteral("none")}};
            // D6, for the raw rows too (Zeb, task 19): another test or formula
            // version than the baseline's measures differently, so a % against
            // it means nothing ("+331%" for a stub's mark against v1's).
            const bool versionDiffers = i != m_baseline && t && base
                                        && (t->testVersion != base->testVersion || t->formulaVersion != base->formulaVersion);
            if (versionDiffers && m) {
                cell.insert(QStringLiteral("changeText"), tr("scoring changed"));
            } else if (i != m_baseline && m && bm && m->isNumeric() && bm->isNumeric() && std::get<double>(bm->value) != 0) {
                const double b = std::get<double>(bm->value);
                const double pct = (std::get<double>(m->value) - b) / std::fabs(b) * 100.0;
                if (std::isfinite(pct)) {  // a NaN metric gets no change %
                    cell.insert(QStringLiteral("changeText"), changeText(pct, lower));
                    cell.insert(QStringLiteral("direction"), dir(pct, lower, neutral));
                }
            }
            cells.append(cell);
        }
        m_rawRows.append(QVariantMap{{QStringLiteral("label"), proto->label}, {QStringLiteral("cells"), cells}});
    }
}

} // namespace sb
