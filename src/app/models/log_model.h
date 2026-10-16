// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/log_line.h"

#include <QAbstractListModel>
#include <QVector>

#include <functional>

namespace sb {

// A run's log lines: the live ring buffer of 5,000 on the Progress screen (§3)
// and a stored run's full log in the View log dialog (D16).
class LogModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role { TimeRole = Qt::UserRole + 1, TestIdRole, TestNameRole, MessageRole, LevelRole, TextRole };

    explicit LogModel(int capacity = 5000, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Maps a test id to its display name (the registry's).
    void setNameResolver(std::function<QString(const QString &)> resolver) { m_names = std::move(resolver); }

    void append(const LogLine &line);
    void setLines(const QVector<LogLine> &lines);  // replaces everything
    void clear();
    const QVector<LogLine> &lines() const { return m_lines; }
    int capacity() const { return m_capacity; }

    // The visible text, for the Copy button: "14:02:01  CPU  message" per line.
    Q_INVOKABLE QString toText(const QString &testFilter = {}, const QString &minLevel = {}) const;

signals:
    void countChanged();

private:
    QString timeText(const LogLine &l) const;

    QVector<LogLine> m_lines;
    int m_capacity;
    std::function<QString(const QString &)> m_names;
};

} // namespace sb
