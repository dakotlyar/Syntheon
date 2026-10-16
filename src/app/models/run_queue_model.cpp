// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/run_queue_model.h"

#include "core/orchestrator.h"

namespace sb {

RunQueueModel::RunQueueModel(QObject *parent) : QAbstractListModel(parent) {}

int RunQueueModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QString RunQueueModel::duration(qint64 ms)
{
    if (ms < 60000)
        return QStringLiteral("%1 s").arg(double(ms) / 1000.0, 0, 'f', 1);
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QVariant RunQueueModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const Row &r = m_rows[index.row()];
    switch (role) {
    case TestIdRole: return r.plan.testId;
    case NameRole: return r.plan.name;
    case ColorTokenRole: return r.plan.colorToken;
    case StateRole: return r.state;
    case FractionRole: return r.fraction;
    case StageRole:
        if (r.state == QLatin1StringView("running") && r.clock.isValid())
            return QStringLiteral("%1 · %2").arg(r.stage, duration(r.clock.elapsed()));
        return r.stage;
    case StateTextRole:
        if (r.state == QLatin1StringView("running"))
            return QStringLiteral("%1%").arg(qRound(r.fraction * 100));
        if (r.state == QLatin1StringView("done"))
            return QStringLiteral("✓ done");
        if (r.state == QLatin1StringView("queued"))
            return QStringLiteral("—");
        if (r.state == QLatin1StringView("na"))
            return QStringLiteral("N/A");
        if (r.state == QLatin1StringView("error"))
            return QStringLiteral("error");
        if (r.state == QLatin1StringView("cancelled"))
            return QStringLiteral("stopped");
        if (r.state == QLatin1StringView("timed_out"))
            return QStringLiteral("timed out");
        return QStringLiteral("not run");
    default: return {};
    }
}

QHash<int, QByteArray> RunQueueModel::roleNames() const
{
    return {{TestIdRole, "testId"}, {NameRole, "name"},   {ColorTokenRole, "colorToken"}, {StateRole, "state"},
            {FractionRole, "fraction"}, {StageRole, "stage"}, {StateTextRole, "stateText"}};
}

void RunQueueModel::setPlan(const QVector<Planned> &plan)
{
    beginResetModel();
    m_rows.clear();
    for (const Planned &p : plan)
        m_rows.append(Row{p});
    endResetModel();
}

void RunQueueModel::changed(int row)
{
    const QModelIndex i = index(row);
    emit dataChanged(i, i);
}

void RunQueueModel::testStarted(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    Row &r = m_rows[index];
    r.state = QStringLiteral("running");
    r.stage = QStringLiteral("Starting");
    r.fraction = 0;
    r.clock.start();
    changed(index);
}

void RunQueueModel::progress(const ProgressUpdate &u)
{
    if (u.index < 0 || u.index >= m_rows.size())
        return;
    Row &r = m_rows[u.index];
    r.fraction = u.fraction;
    r.stage = u.stage;
    changed(u.index);
}

void RunQueueModel::testFinished(int index, const TestResult &result)
{
    if (index < 0 || index >= m_rows.size())
        return;
    Row &r = m_rows[index];
    r.fraction = result.status == Status::Ok || result.status == Status::Partial ? 1.0 : r.fraction;
    r.durationMs = result.durationMs;
    switch (result.status) {
    case Status::Ok:
    case Status::Partial:
        r.state = QStringLiteral("done");
        r.stage = QStringLiteral("Done · %1").arg(duration(result.durationMs));
        break;
    case Status::NotAvailable:
        r.state = QStringLiteral("na");
        r.stage = result.statusReason;
        break;
    case Status::Error:
        r.state = QStringLiteral("error");
        r.stage = result.statusReason;
        break;
    case Status::Cancelled:
        r.state = QStringLiteral("cancelled");
        r.stage = QStringLiteral("Stopped · %1").arg(duration(result.durationMs));
        break;
    case Status::TimedOut:
        r.state = QStringLiteral("timed_out");
        r.stage = result.statusReason;
        break;
    case Status::Interrupted:
        r.state = QStringLiteral("interrupted");
        r.stage = result.statusReason;
        break;
    case Status::NotRun:
        r.state = QStringLiteral("not_run");
        r.stage = QStringLiteral("Not run");
        break;
    }
    r.clock.invalidate();
    changed(index);
}

void RunQueueModel::tick()
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].state == QLatin1StringView("running"))
            changed(i);
    }
}

} // namespace sb
