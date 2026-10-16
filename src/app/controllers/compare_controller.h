// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/result.h"

#include <QObject>
#include <QVariantList>

#include <optional>

namespace sb {

class RunController;
class Storage;
class StorageController;

// QML singleton "Compare": N runs side by side (D5). One column per run, the
// change in % measured against a baseline the user picks (default: the newest
// run). A row whose test or formula version differs between the runs is
// marked "scoring changed" while the raw metrics are still compared (D6); a
// Stopped run's overall is marked partial (N2). Comparison is local only.
class CompareController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool valid READ valid NOTIFY changed)
    Q_PROPERTY(QString subtitle READ subtitle NOTIFY changed)
    Q_PROPERTY(QVariantList runs READ runs NOTIFY changed)
    Q_PROPERTY(QString baselineUuid READ baselineUuid NOTIFY changed)
    Q_PROPERTY(QVariantList scoreRows READ scoreRows NOTIFY changed)
    Q_PROPERTY(QVariantList rawTests READ rawTests NOTIFY changed)
    Q_PROPERTY(QString rawTestId READ rawTestId NOTIFY rawChanged)
    Q_PROPERTY(QString rawTestName READ rawTestName NOTIFY rawChanged)
    Q_PROPERTY(QVariantList rawRows READ rawRows NOTIFY rawChanged)

public:
    CompareController(RunController &run, Storage &storage, StorageController &storageController, QObject *parent = nullptr);

    bool valid() const { return m_runs.size() >= 2; }
    QString subtitle() const;
    QVariantList runs() const { return m_runColumns; }
    QString baselineUuid() const;
    QVariantList scoreRows() const { return m_scoreRows; }
    QVariantList rawTests() const { return m_rawTests; }
    QString rawTestId() const { return m_rawTestId; }
    QString rawTestName() const;
    QVariantList rawRows() const { return m_rawRows; }

    // Loads the runs (uuids, any order; they're shown newest first). False if
    // fewer than two exist.
    Q_INVOKABLE bool setRuns(const QStringList &uuids);
    Q_INVOKABLE void setBaseline(const QString &uuid);
    Q_INVOKABLE void setRawTest(const QString &testId);

    // "+36%", "-12%", "0%"; "+52% (worse)" for a lower-is-better rise.
    static QString changeText(double pct, bool lowerIsBetter);

signals:
    void changed();
    void rawChanged();

private:
    void rebuild();
    void rebuildRaw();
    const TestResult *resultOf(const RunRecord &run, const QString &testId) const;

    RunController &m_run;
    Storage &m_storage;
    StorageController &m_storageController;
    QVector<RunRecord> m_runs;  // newest first
    int m_baseline = 0;         // index into m_runs
    QVariantList m_runColumns, m_scoreRows, m_rawTests, m_rawRows;
    QString m_rawTestId;
};

} // namespace sb
