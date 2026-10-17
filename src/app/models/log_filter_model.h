// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSortFilterProxyModel>

namespace sb {

// The View log dialog's filter (D16): by test and by minimum level.
class LogFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QString testFilter READ testFilter WRITE setTestFilter NOTIFY filterChanged)
    Q_PROPERTY(QString minLevel READ minLevel WRITE setMinLevel NOTIFY filterChanged)
    // The visible lines, for the dialog's "N lines" (a binding to rowCount()
    // would never update).
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit LogFilterModel(QObject *parent = nullptr);

    QString testFilter() const { return m_test; }
    QString minLevel() const { return m_level; }
    void setTestFilter(const QString &id);
    void setMinLevel(const QString &level);  // "" | "info" | "warning" | "error"
    int count() const { return rowCount(); }

    // The visible lines as text, for Copy.
    Q_INVOKABLE QString visibleText() const;

signals:
    void filterChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
    QString m_test, m_level;
};

} // namespace sb
