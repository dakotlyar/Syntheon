// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/run_repository.h"

#include <QAbstractListModel>
#include <QSet>
#include <QStringList>

namespace sb {

class RunController;
class Storage;
class StorageController;

// QML singleton "History": every saved run, newest first, in pages of 200
// (§7). Tick two or more to compare. Rows are plain values (never pointers
// into the database).
class HistoryModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int totalCount READ totalCount NOTIFY totalChanged)
    Q_PROPERTY(int checkedCount READ checkedCount NOTIFY checkedChanged)
    Q_PROPERTY(QString compareText READ compareText NOTIFY checkedChanged)
    Q_PROPERTY(bool canCompare READ canCompare NOTIFY checkedChanged)

public:
    enum Role {
        UuidRole = Qt::UserRole + 1,
        DateRole,
        MachineRole,
        TestsRole,       // "CPU, RAM, Storage" / "Battery (Mixed)"
        OverallRole,     // "4,812", "6 h 42 m" for a Battery run, "—"
        PartialRole,     // N2: a Stopped run's overall is partial
        StatusRole,      // "", "Stopped", "Interrupted", "Recovered"
        CheckedRole,
    };
    static constexpr int kPageSize = 200;

    HistoryModel(RunController &run, Storage &storage, StorageController &storageController, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

    int totalCount() const { return m_total; }
    int checkedCount() const { return int(m_checked.size()); }
    QString compareText() const;
    bool canCompare() const { return m_checked.size() >= 2; }
    // The ticked runs, newest first.
    Q_INVOKABLE QStringList checkedUuids() const;

    Q_INVOKABLE void toggleChecked(const QString &uuid);
    Q_INVOKABLE void clearChecked();
    // Starts over from the newest run (after a run finished, a clear, a move).
    Q_INVOKABLE void reload();

signals:
    void totalChanged();
    void checkedChanged();

private:
    QString testsText(const RunRepository::RunSummary &s) const;
    QString overallText(const RunRepository::RunSummary &s) const;
    int rowOf(const QString &uuid) const;

    RunController &m_run;
    Storage &m_storage;
    StorageController &m_storageController;
    QVector<RunRepository::RunSummary> m_rows;
    QSet<QString> m_checked;
    int m_total = 0;
    int m_fetched = 0;  // rows read from the database so far (shown + dropped duplicates)
};

} // namespace sb
