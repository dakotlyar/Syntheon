// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/models/log_model.h"
#include "app/models/options_model.h"
#include "app/models/run_queue_model.h"
#include "storage/run_repository.h"
#include "tests/common/stub_clock.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace sb;

class TestRunController : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<AppRuntime> m_rt;

    RunController &rc() { return m_rt->run(); }

    void startRuntime()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        AppRuntime::Options o;
        o.smoke = true;
        o.settingsFile = m_dir->filePath(QStringLiteral("settings.ini"));
        o.dataFolder = m_dir->filePath(QStringLiteral("data"));
        m_rt = std::make_unique<AppRuntime>(o);
        m_rt->storageController().open();
        m_rt->startup().waitForRecovery();
        m_rt->app().start();  // no elevation: the gate opens at once
        rc().refreshProbes();
        QTRY_VERIFY_WITH_TIMEOUT(!rc().probing(), 20000);
    }

    QStringList queueStates()
    {
        auto *q = qobject_cast<RunQueueModel *>(rc().queue());
        QStringList out;
        for (int i = 0; i < q->rowCount(); ++i)
            out.append(q->data(q->index(i), RunQueueModel::StateRole).toString());
        return out;
    }

private slots:
    // AP-5 #5: a refused Start says why (and the reason clears on the next edit).
    void refusedStartExposesItsReason()
    {
        startRuntime();
        rc().toggle(QStringLiteral("cpu"));
        QSignalSpy changed(&rc(), &RunController::startErrorsChanged);
        m_rt->storageController().lock(QStringLiteral("recovery"), QStringLiteral("checking"));
        QVERIFY(!rc().canStart());
        QVERIFY(!rc().start());
        QVERIFY(!rc().lastStartErrors().isEmpty());
        QVERIFY(changed.count() >= 1);
        rc().toggle(QStringLiteral("ram"));  // any edit clears it
        QVERIFY(rc().lastStartErrors().isEmpty());
        m_rt->storageController().unlock(QStringLiteral("recovery"));
        QVERIFY(rc().canStart());
    }

    void init() { tests::setStubTimeScale(0.03); }
    void cleanup()
    {
        m_rt.reset();
        m_dir.reset();
        tests::setStubTimeScale(0);
    }

    void probesFillAvailabilityAndOptions()
    {
        startRuntime();
        QCOMPARE(rc().selection().rowCount(), 7);
        const QModelIndex npu = rc().selection().index(5);
        QCOMPARE(rc().selection().data(npu, SelectionModel::AvailableRole).toBool(), false);
        QVERIFY(rc().selection().data(npu, SelectionModel::UnavailableReasonRole).toString().contains(QLatin1StringView("NPU")));
        rc().toggle(QStringLiteral("npu"));
        QCOMPARE(rc().selectedCount(), 0);  // N/A can't be ticked

        rc().toggle(QStringLiteral("ram"));
        QCOMPARE(rc().activeTab(), QStringLiteral("ram"));
        auto *options = qobject_cast<OptionsModel *>(rc().options());
        QVERIFY(options);
        QAbstractItemModelTester tester(options, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(options->rowCount() >= 6);  // sub-tests, buffer, duration, memory, simulate, seed
        QCOMPARE(options->value(QStringLiteral("buffer")).toString(), QStringLiteral("1g"));
        QCOMPARE(options->value(QStringLiteral("subtests")).toStringList().size(), 3);
    }

    void exclusivityAndTexts()
    {
        startRuntime();
        QCOMPARE(rc().runButtonText(), QStringLiteral("Run selected tests"));
        QVERIFY(rc().estimateText().isEmpty());
        rc().toggle(QStringLiteral("cpu"));
        rc().toggle(QStringLiteral("gpu"));
        QVERIFY(rc().estimateText().startsWith(QLatin1StringView("estimated")));
        QCOMPARE(rc().tabs().size(), 2);
        rc().toggle(QStringLiteral("battery"));
        QCOMPARE(rc().selectedCount(), 1);
        QCOMPARE(rc().runButtonText(), QStringLiteral("Start battery run"));
        QVERIFY(rc().estimateText().contains(QLatin1StringView("battery is empty")));
        QVERIFY(rc().estimateOf(QStringLiteral("cpu")).startsWith(QLatin1Char('~')));
    }

    void optionEditsReachTheRun()
    {
        startRuntime();
        rc().toggle(QStringLiteral("ram"));
        const QString before = rc().estimateText();
        rc().setOption(QStringLiteral("ram"), QStringLiteral("duration"), 60);
        QVERIFY(rc().estimateText() != before);
        QCOMPARE(rc().valuesFor(QStringLiteral("ram")).value(QStringLiteral("duration")).toDouble(), 60.0);
        rc().setOption(QStringLiteral("ram"), QStringLiteral("duration"), 9999);  // clamped
        QCOMPARE(rc().valuesFor(QStringLiteral("ram")).value(QStringLiteral("duration")).toDouble(), 60.0);
        rc().setOption(QStringLiteral("ram"), QStringLiteral("subtests"), QVariantList{QStringLiteral("latency")});
        QCOMPARE(rc().valuesFor(QStringLiteral("ram")).value(QStringLiteral("subtests")).toStringList(), QStringList{QStringLiteral("latency")});
        // An invalid choice is a problem that blocks Start, in words.
        rc().setOption(QStringLiteral("ram"), QStringLiteral("subtests"), QVariantList{});
        QVERIFY(!rc().canStart());
        QVERIFY(rc().problems().join(u' ').contains(QLatin1StringView("sub-test")));
    }

    void aRunFlowsThroughTheModelsAndLocksStorage()
    {
        startRuntime();
        rc().toggle(QStringLiteral("cpu"));
        rc().toggle(QStringLiteral("ram"));
        QVERIFY(rc().canStart());
        QSignalSpy started(&rc(), &RunController::runStarted);
        QSignalSpy finished(&rc(), &RunController::runFinished);
        QVERIFY(rc().start());
        QCOMPARE(started.count(), 1);
        QVERIFY(rc().running());
        QCOMPARE(m_rt->app().section(), QStringLiteral("progress"));
        QVERIFY(m_rt->storageController().locked());  // no location change or Clear history during a run
        QVERIFY(!rc().canStart());                    // "a run is already in progress"
        QCOMPARE(queueStates().size(), 2);
        QCOMPARE(rc().runTitle(), QStringLiteral("Running 2 tests"));

        QVERIFY(finished.wait(60000));
        QCOMPARE(finished.last().at(1).toString(), QStringLiteral("completed"));
        QVERIFY(!rc().running());
        QVERIFY(!m_rt->storageController().locked());
        QCOMPARE(queueStates(), (QStringList{QStringLiteral("done"), QStringLiteral("done")}));
        auto *q = qobject_cast<RunQueueModel *>(rc().queue());
        QVERIFY(q->data(q->index(0), RunQueueModel::StageRole).toString().startsWith(QLatin1StringView("Done")));
        QCOMPARE(q->data(q->index(0), RunQueueModel::StateTextRole).toString(), QStringLiteral("✓ done"));

        auto *log = qobject_cast<LogModel *>(rc().log());
        QVERIFY(log->rowCount() >= 4);
        QVERIFY(log->toText().contains(QLatin1StringView("CPU")));
        QVERIFY(log->toText(QStringLiteral("ram")).contains(QLatin1StringView("RAM")));
        QVERIFY(!log->toText(QStringLiteral("ram")).contains(QLatin1StringView("CPU")));
        QCOMPARE(m_rt->storageController().runCount(), 1);

        // And a second run works.
        finished.clear();
        QVERIFY(rc().start());
        QVERIFY(finished.wait(60000));
        QCOMPARE(m_rt->storageController().runCount(), 2);
    }

    void stopSavesAStoppedRun()
    {
        tests::setStubTimeScale(1.0);
        startRuntime();
        rc().toggle(QStringLiteral("cpu"));
        rc().toggle(QStringLiteral("ram"));
        QSignalSpy finished(&rc(), &RunController::runFinished);
        QVERIFY(rc().start());
        QTRY_COMPARE_WITH_TIMEOUT(queueStates().value(0), QStringLiteral("running"), 5000);
        QTest::qWait(300);
        rc().stop();
        QVERIFY(rc().stopping() || !rc().running());
        QVERIFY(finished.wait(20000));
        QCOMPARE(finished.last().at(1).toString(), QStringLiteral("stopped"));
        QCOMPARE(queueStates(), (QStringList{QStringLiteral("cancelled"), QStringLiteral("not_run")}));
        QCOMPARE(m_rt->storageController().runCount(), 1);  // saved, D3
    }

    void startIsRefusedWithReasons()
    {
        startRuntime();
        rc().toggle(QStringLiteral("battery"));
        QTRY_VERIFY_WITH_TIMEOUT(!rc().preconditions().isEmpty(), 5000);
        // A machine with no battery (or on AC) can't start it...
        QVERIFY(!rc().canStart());
        QVERIFY(!rc().problems().isEmpty());
        QSignalSpy refused(&rc(), &RunController::startRefused);
        QVERIFY(!rc().start());
        QCOMPARE(refused.count(), 1);
        QVERIFY(!rc().lastStartErrors().isEmpty());
        QVERIFY(!rc().running());
        // ...until the Phase 1 override is ticked.
        rc().setOption(QStringLiteral("battery"), QStringLiteral("override_preconditions"), true);
        QTRY_VERIFY_WITH_TIMEOUT(rc().canStart(), 5000);
    }

    void etaAndTitles()
    {
        tests::setStubTimeScale(1.0);
        startRuntime();
        rc().toggle(QStringLiteral("cpu"));
        QSignalSpy finished(&rc(), &RunController::runFinished);
        QVERIFY(rc().start());
        QTRY_VERIFY_WITH_TIMEOUT(rc().runSubtitle().contains(QLatin1StringView("left")), 5000);
        QVERIFY(rc().runSubtitle().startsWith(QLatin1StringView("Started")));
        QCOMPARE(rc().runTitle(), QStringLiteral("Running CPU"));
        rc().stop();
        QVERIFY(finished.wait(20000));
        QCOMPARE(rc().runTitle(), QStringLiteral("Run of CPU"));
        QVERIFY(!rc().runSubtitle().contains(QLatin1StringView("left")));
    }
};

QTEST_MAIN(TestRunController)
#include "tst_run_controller.moc"
