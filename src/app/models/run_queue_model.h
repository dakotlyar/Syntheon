// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/result.h"

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QVector>

namespace sb {

struct ProgressUpdate;

// The Progress screen's queue: one row per planned test with its state,
// fraction, stage and elapsed time (§3).
class RunQueueModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        TestIdRole = Qt::UserRole + 1,
        NameRole,
        ColorTokenRole,
        StateRole,      // queued | running | done | na | error | cancelled | timed_out | interrupted | not_run
        FractionRole,
        StageRole,      // "Loaded latency", "Queued", "Done · 3:02", the reason for N/A / error
        StateTextRole,  // "62%", "✓ done", "—"
    };

    explicit RunQueueModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    struct Planned {
        QString testId, name, colorToken;
    };
    void setPlan(const QVector<Planned> &plan);
    void testStarted(int index);
    void progress(const ProgressUpdate &update);
    void testFinished(int index, const TestResult &result);
    // Refreshes the running row's elapsed time.
    void tick();

    static QString duration(qint64 ms);  // "3:02" / "9.6 s"

private:
    struct Row {
        Planned plan;
        QString state = QStringLiteral("queued");
        double fraction = 0;
        QString stage = QStringLiteral("Queued");
        qint64 durationMs = 0;
        QElapsedTimer clock;
    };
    void changed(int row);

    QVector<Row> m_rows;
};

} // namespace sb
