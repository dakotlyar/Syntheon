// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/log_filter_model.h"

#include "app/models/log_model.h"

namespace sb {

namespace {
int rank(const QString &level)
{
    return level == QLatin1StringView("error") ? 3 : level == QLatin1StringView("warning") ? 2
           : level == QLatin1StringView("info") ? 1 : 0;
}
} // namespace

LogFilterModel::LogFilterModel(QObject *parent) : QSortFilterProxyModel(parent) {}

void LogFilterModel::setTestFilter(const QString &id)
{
    if (id == m_test)
        return;
    beginFilterChange();
    m_test = id;
    endFilterChange();
    emit filterChanged();
}

void LogFilterModel::setMinLevel(const QString &level)
{
    if (level == m_level)
        return;
    beginFilterChange();
    m_level = level;
    endFilterChange();
    emit filterChanged();
}

bool LogFilterModel::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    const QModelIndex i = sourceModel()->index(row, 0, parent);
    if (!m_test.isEmpty() && sourceModel()->data(i, LogModel::TestIdRole).toString() != m_test)
        return false;
    return rank(sourceModel()->data(i, LogModel::LevelRole).toString()) >= rank(m_level);
}

QString LogFilterModel::visibleText() const
{
    QStringList out;
    for (int i = 0; i < rowCount(); ++i)
        out.append(data(index(i, 0), LogModel::TextRole).toString());
    return out.join(QLatin1Char('\n'));
}

} // namespace sb
