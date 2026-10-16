// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/log_model.h"

#include <QTimeZone>

namespace sb {

LogModel::LogModel(int capacity, QObject *parent) : QAbstractListModel(parent), m_capacity(capacity) {}

int LogModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_lines.size());
}

QString LogModel::withoutNamePrefix(const QString &message, const QString &name)
{
    if (name.isEmpty())
        return message;
    for (const QString &prefix : {name + QStringLiteral(": "), name + QStringLiteral(" - ")}) {
        if (message.startsWith(prefix, Qt::CaseInsensitive) && message.size() > prefix.size())
            return message.mid(prefix.size());
    }
    return message;
}

QString LogModel::timeText(const LogLine &l) const
{
    return l.timestamp.toLocalTime().toString(QStringLiteral("HH:mm:ss"));
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const LogLine &l = m_lines[index.row()];
    const QString name = l.testId.isEmpty() ? QString() : (m_names ? m_names(l.testId) : l.testId);
    switch (role) {
    case TimeRole: return timeText(l);
    case TestIdRole: return l.testId;
    case TestNameRole: return name;
    case MessageRole: return l.message;
    case LevelRole: return toKey(l.level);
    case BodyRole: return withoutNamePrefix(l.message, name);
    case TestColorRole: return l.testId.isEmpty() || !m_colors ? QString() : m_colors(l.testId);
    case TextRole:
        return QStringLiteral("%1  %2  %3").arg(timeText(l), name.leftJustified(8), l.message).trimmed();
    default: return {};
    }
}

QHash<int, QByteArray> LogModel::roleNames() const
{
    return {{TimeRole, "time"},       {TestIdRole, "testId"}, {TestNameRole, "testName"},
            {MessageRole, "message"}, {LevelRole, "level"},   {TextRole, "text"},
            {BodyRole, "body"},       {TestColorRole, "testColor"}};
}

void LogModel::append(const LogLine &line)
{
    if (m_lines.size() >= m_capacity) {
        beginRemoveRows({}, 0, 0);
        m_lines.removeFirst();
        endRemoveRows();
    }
    beginInsertRows({}, int(m_lines.size()), int(m_lines.size()));
    m_lines.append(line);
    endInsertRows();
    emit countChanged();
}

void LogModel::setLines(const QVector<LogLine> &lines)
{
    beginResetModel();
    m_lines = lines;
    endResetModel();
    emit countChanged();
}

void LogModel::clear()
{
    setLines({});
}

QString LogModel::toText(const QString &testFilter, const QString &minLevel) const
{
    const auto rank = [](const QString &level) {
        return level == QLatin1StringView("error") ? 3 : level == QLatin1StringView("warning") ? 2
               : level == QLatin1StringView("info") ? 1 : 0;
    };
    const int minRank = rank(minLevel);
    QStringList out;
    for (int i = 0; i < m_lines.size(); ++i) {
        const LogLine &l = m_lines[i];
        if (!testFilter.isEmpty() && l.testId != testFilter)
            continue;
        if (rank(toKey(l.level)) < minRank)
            continue;
        out.append(data(index(i), TextRole).toString());
    }
    return out.join(QLatin1Char('\n'));
}

} // namespace sb
