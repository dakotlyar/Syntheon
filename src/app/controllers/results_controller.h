// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/result.h"

#include <QObject>
#include <QVariantList>

#include <memory>
#include <optional>

namespace sb {

class LogFilterModel;
class LogModel;
class RunController;
class Storage;
class StorageController;

// QML singleton "Results": the run shown on the Results screen and the test
// shown on its Detail page. It loads a stored run (or follows the one in
// progress, "View results so far") and turns it into plain QML data.
class ResultsController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool valid READ valid NOTIFY runChanged)
    Q_PROPERTY(QString uuid READ uuid NOTIFY runChanged)
    Q_PROPERTY(bool live READ live NOTIFY runChanged)
    Q_PROPERTY(QString title READ title NOTIFY runChanged)
    Q_PROPERTY(QString subtitle READ subtitle NOTIFY runChanged)
    Q_PROPERTY(QString statusKey READ statusKey NOTIFY runChanged)
    Q_PROPERTY(QString statusLabel READ statusLabel NOTIFY runChanged)
    Q_PROPERTY(QString overallText READ overallText NOTIFY runChanged)
    Q_PROPERTY(bool overallPartial READ overallPartial NOTIFY runChanged)
    Q_PROPERTY(double scoredFraction READ scoredFraction NOTIFY runChanged)
    Q_PROPERTY(QString overallNote READ overallNote NOTIFY runChanged)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY runChanged)
    // Detail
    Q_PROPERTY(QVariantList detailTabs READ detailTabs NOTIFY runChanged)
    Q_PROPERTY(QString detailTestId READ detailTestId NOTIFY detailChanged)
    Q_PROPERTY(QString detailTitle READ detailTitle NOTIFY detailChanged)
    Q_PROPERTY(QString detailSubtitle READ detailSubtitle NOTIFY detailChanged)
    Q_PROPERTY(QString detailNote READ detailNote NOTIFY detailChanged)
    Q_PROPERTY(QString detailColorToken READ detailColorToken NOTIFY detailChanged)
    Q_PROPERTY(QVariantList detailBlocks READ detailBlocks NOTIFY detailChanged)
    Q_PROPERTY(bool detailOpen READ detailOpen NOTIFY detailOpenChanged)

public:
    ResultsController(RunController &run, Storage &storage, StorageController &storageController, QObject *parent = nullptr);
    ~ResultsController() override;

    bool valid() const { return m_run.has_value(); }
    QString uuid() const;
    bool live() const { return m_live; }
    QString title() const;
    QString subtitle() const;
    QString statusKey() const;
    QString statusLabel() const;
    QString overallText() const;
    bool overallPartial() const;
    double scoredFraction() const;
    QString overallNote() const;
    QVariantList rows() const { return m_rows; }
    QVariantList detailTabs() const { return m_tabs; }
    QString detailTestId() const { return m_detailId; }
    QString detailTitle() const { return m_detailTitle; }
    QString detailSubtitle() const { return m_detailSubtitle; }
    QString detailNote() const { return m_detailNote; }
    QString detailColorToken() const { return m_detailColor; }
    QVariantList detailBlocks() const { return m_detailBlocks; }

    const std::optional<RunRecord> &run() const { return m_run; }

    // Loads a stored run by uuid ("" = the newest). False if it isn't there.
    Q_INVOKABLE bool openRun(const QString &uuid);
    // The run in progress, or the one that just finished ("View results so far").
    Q_INVOKABLE void openCurrent();
    Q_INVOKABLE void selectDetail(const QString &testId);
    // Opens (or closes) the Detail sub-page of Results.
    bool detailOpen() const { return m_detailOpen; }
    Q_INVOKABLE void showDetail(const QString &testId);
    Q_INVOKABLE void closeDetail();
    // The stored log of the open run, as a filterable model (D16, View log).
    Q_INVOKABLE QObject *logModel();
    Q_INVOKABLE QVariantList logTests() const;  // [{id, name}] for the filter

    static QString formatDate(const QDateTime &t);
    static QString statusLabelFor(RunStatus s);

signals:
    void runChanged();
    void detailChanged();
    void detailOpenChanged();
    void logChanged();

private:
    void setRun(RunRecord run, bool live);
    void rebuild();
    void buildDetail();
    QString scoreText(const TestResult &t) const;
    QString rawSummary(const TestResult &t) const;
    std::optional<double> overallValue() const;

    RunController &m_runController;
    Storage &m_storage;
    StorageController &m_storageController;
    std::optional<RunRecord> m_run;
    bool m_live = false;
    QVariantList m_rows, m_tabs;
    bool m_detailOpen = false;
    QString m_detailId, m_detailTitle, m_detailSubtitle, m_detailNote, m_detailColor;
    QVariantList m_detailBlocks;
    std::optional<RunRecord> m_batteryRun;  // the Detail Battery tab's own run
    std::unique_ptr<LogModel> m_logSource;
    std::unique_ptr<LogFilterModel> m_logFilter;
};

} // namespace sb
