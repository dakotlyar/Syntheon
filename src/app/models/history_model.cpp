// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/history_model.h"

#include "app/controllers/results_controller.h"
#include "app/controllers/run_controller.h"
#include "app/controllers/storage_controller.h"
#include "core/units.h"
#include "storage/storage.h"

#include <algorithm>
#include <cmath>

namespace sb {

HistoryModel::HistoryModel(RunController &run, Storage &storage, StorageController &storageController, QObject *parent)
    : QAbstractListModel(parent), m_run(run), m_storage(storage), m_storageController(storageController)
{
    connect(&m_storageController, &StorageController::historyChanged, this, &HistoryModel::reload);
    connect(&m_run, &RunController::runFinished, this, &HistoryModel::reload);
}

int HistoryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QString HistoryModel::testsText(const RunRepository::RunSummary &s) const
{
    if (s.kind == RunKind::Exclusive && s.testIds.size() == 1 && s.testIds.first() == QLatin1StringView("battery")) {
        const QString p = s.batteryProfile;
        return p.isEmpty() ? tr("Battery") : tr("Battery (%1)").arg(p.left(1).toUpper() + p.mid(1));
    }
    QStringList names;
    for (const QString &id : s.testIds)
        names.append(m_run.testName(id));
    return names.join(QStringLiteral(", "));
}

QString HistoryModel::overallText(const RunRepository::RunSummary &s) const
{
    // A Battery run's headline is its runtime, not a score (it doesn't count
    // toward the overall until Phase 8 decides).
    if (!s.overall && s.batteryMinutes) {
        const qint64 m = qint64(std::llround(*s.batteryMinutes));
        return QStringLiteral("%1 h %2 m").arg(m / 60).arg(m % 60, 2, 10, QLatin1Char('0'));
    }
    return s.overall ? Units::format(*s.overall, QStringLiteral("points"), false) : QStringLiteral("—");
}

QVariant HistoryModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const RunRepository::RunSummary &s = m_rows[index.row()];
    switch (role) {
    case UuidRole: return s.uuid.toString(QUuid::WithoutBraces);
    case DateRole: return ResultsController::formatDate(s.startedAt);
    case MachineRole: return s.machineLabel;
    case TestsRole: return testsText(s);
    case OverallRole: return overallText(s);
    case PartialRole: return s.status == RunStatus::Stopped && s.overall.has_value();  // N2
    case StatusRole: return ResultsController::statusLabelFor(s.status);
    case CheckedRole: return m_checked.contains(s.uuid.toString(QUuid::WithoutBraces));
    default: return {};
    }
}

QHash<int, QByteArray> HistoryModel::roleNames() const
{
    return {{UuidRole, "uuid"},       {DateRole, "date"},         {MachineRole, "machine"}, {TestsRole, "tests"},
            {OverallRole, "overall"}, {PartialRole, "partial"},   {StatusRole, "status"},   {CheckedRole, "checked"}};
}

bool HistoryModel::canFetchMore(const QModelIndex &parent) const
{
    return !parent.isValid() && m_rows.size() < m_total;
}

void HistoryModel::fetchMore(const QModelIndex &parent)
{
    if (parent.isValid() || !m_storage.isOpen())
        return;
    RunRepository repo(m_storage.connection());
    const auto page = repo.listRuns(int(m_rows.size()), kPageSize);
    if (page.isEmpty()) {
        m_total = int(m_rows.size());  // the history shrank under us
        emit totalChanged();
        return;
    }
    beginInsertRows({}, int(m_rows.size()), int(m_rows.size() + page.size() - 1));
    m_rows += page;
    endInsertRows();
}

void HistoryModel::reload()
{
    beginResetModel();
    m_rows.clear();
    m_total = 0;
    if (m_storage.isOpen()) {
        RunRepository repo(m_storage.connection());
        m_total = std::max(0, repo.runCount());
        m_rows = repo.listRuns(0, kPageSize);
    }
    // Drop ticks of runs that no longer exist.
    QSet<QString> alive;
    if (m_storage.isOpen()) {
        RunRepository repo(m_storage.connection());
        for (const QString &id : std::as_const(m_checked)) {
            if (repo.runId(QUuid::fromString(id)))
                alive.insert(id);
        }
    }
    m_checked = alive;
    endResetModel();
    emit totalChanged();
    emit checkedChanged();
}

int HistoryModel::rowOf(const QString &uuid) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].uuid.toString(QUuid::WithoutBraces) == uuid)
            return i;
    }
    return -1;
}

void HistoryModel::toggleChecked(const QString &uuid)
{
    if (m_checked.contains(uuid))
        m_checked.remove(uuid);
    else
        m_checked.insert(uuid);
    const int row = rowOf(uuid);
    if (row >= 0)
        emit dataChanged(index(row), index(row), {CheckedRole});
    emit checkedChanged();
}

void HistoryModel::clearChecked()
{
    if (m_checked.isEmpty())
        return;
    m_checked.clear();
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(int(m_rows.size()) - 1), {CheckedRole});
    emit checkedChanged();
}

QString HistoryModel::compareText() const
{
    const int n = checkedCount();
    return n >= 2 ? tr("Compare %1 runs").arg(n) : tr("Pick 2 or more");
}

QStringList HistoryModel::checkedUuids() const
{
    // Newest first, like the table: by the row order, falling back to the set.
    QStringList out;
    for (const RunRepository::RunSummary &s : m_rows) {
        const QString id = s.uuid.toString(QUuid::WithoutBraces);
        if (m_checked.contains(id))
            out.append(id);
    }
    for (const QString &id : m_checked) {
        if (!out.contains(id))
            out.append(id);  // ticked on a page no longer loaded
    }
    return out;
}

} // namespace sb
