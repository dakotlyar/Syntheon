// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "tests/gpu/client/gpu_client.h"
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
        // The GPU test's worker is a fake on every OS (it answers the probe with one GPU), so these
        // tests don't depend on the machine's GPU or its drivers (Windows CI has no software GPU).
        sb::gpu::GpuClient::setWorkerForTests(QStringLiteral(SB_FAKE_GPUWORKER), {QStringLiteral("ok")});
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
    // Zeb Z3 (48b6f17): the scratch root is copied from the config before the
    // orchestrator's initialiser moves the config; it used to read the
    // moved-from (empty) string.
    void scratchRootIsTheConfigs()
    {
        if (!m_rt)
            startRuntime();
        Orchestrator::Config config;
        config.scratchRoot = m_dir->filePath(QStringLiteral("scratch-root"));
        RunController controller(m_rt->storage(), m_rt->storageController(), m_rt->app(), config);
        QCOMPARE(controller.scratchRoot(), config.scratchRoot);
        QVERIFY(!controller.scratchRoot().isEmpty());
    }

    // Zeb #4: the live log shows the test in its own column, so its message
    // drops a leading "<name>: " (only that; anything else is left alone).
    void logBodyDropsTheTestNamePrefix()
    {
        QCOMPARE(LogModel::withoutNamePrefix(QStringLiteral("GPU: Native 2D"), QStringLiteral("GPU")), QStringLiteral("Native 2D"));
        QCOMPARE(LogModel::withoutNamePrefix(QStringLiteral("AI inference: LLM text generation"), QStringLiteral("AI inference")),
                 QStringLiteral("LLM text generation"));
        QCOMPARE(LogModel::withoutNamePrefix(QStringLiteral("Starting AI inference"), QStringLiteral("AI inference")),
                 QStringLiteral("Starting AI inference"));
        QCOMPARE(LogModel::withoutNamePrefix(QStringLiteral("GPU: "), QStringLiteral("GPU")), QStringLiteral("GPU: "));
        QCOMPARE(LogModel::withoutNamePrefix(QStringLiteral("x"), QString()), QStringLiteral("x"));
        LogModel m;
        m.setNameResolver([](const QString &) { return QStringLiteral("GPU"); });
        m.setColorResolver([](const QString &) { return QStringLiteral("t-gpu"); });
        LogLine l;
        l.testId = QStringLiteral("gpu");
        l.message = QStringLiteral("GPU: Vulkan 3D");
        m.append(l);
        QCOMPARE(m.data(m.index(0), LogModel::BodyRole).toString(), QStringLiteral("Vulkan 3D"));
        QCOMPARE(m.data(m.index(0), LogModel::MessageRole).toString(), QStringLiteral("GPU: Vulkan 3D"));  // stored as is
        QCOMPARE(m.data(m.index(0), LogModel::TestColorRole).toString(), QStringLiteral("t-gpu"));
    }
    // AP-5 #11: a ticked MultiSelect entry that has gone away or been disabled
    // is dropped from the value when the schema is refreshed (SingleSelect
    // already was).
    void multiSelectIsPrunedWhenAnItemGoesAway()
    {
        OptionDef d;
        d.key = QStringLiteral("runtimes");
        d.kind = OptionKind::MultiSelect;
        d.items = {{QStringLiteral("a"), QStringLiteral("A")}, {QStringLiteral("b"), QStringLiteral("B")}, {QStringLiteral("c"), QStringLiteral("C")}};
        d.defaultValue = QStringList{QStringLiteral("a")};
        OptionSchema first;
        first.options = {d};
        OptionValues values;
        OptionsModel model(first, &values);
        model.setSchema(first);
        model.setValue(QStringLiteral("runtimes"), QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
        QCOMPARE(values.value(QStringLiteral("runtimes")).toStringList(), (QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));

        OptionDef d2 = d;
        d2.items[1].enabled = false;        // b: now disabled
        d2.items.removeAt(2);               // c: gone
        OptionSchema second;
        second.options = {d2};
        model.setSchema(second);
        QCOMPARE(values.value(QStringLiteral("runtimes")).toStringList(), (QStringList{QStringLiteral("a")}));
    }

    // DEF-3: an Info row that follows a select shows the selected value's text, and updates when the
    // selection changes (the GPU test's APIs row: the picked GPU's versions, not the default's).
    void infoRowFollowsItsSelect()
    {
        OptionDef gpu;
        gpu.key = QStringLiteral("gpu");
        gpu.kind = OptionKind::DynamicSelect;
        gpu.items = {{QStringLiteral("nv"), QStringLiteral("NVIDIA")}, {QStringLiteral("amd"), QStringLiteral("610M")}};
        gpu.defaultValue = QStringLiteral("nv");
        OptionDef apis;
        apis.key = QStringLiteral("apis");
        apis.kind = OptionKind::Info;
        apis.infoValue = QStringLiteral("Vulkan 1.4.351");
        apis.infoFollows = QStringLiteral("gpu");
        apis.infoByValue = {{QStringLiteral("nv"), QStringLiteral("Vulkan 1.4.351")}, {QStringLiteral("amd"), QStringLiteral("Vulkan 1.4.318 · N/A: DirectX is Windows only")}};
        OptionSchema schema;
        schema.options = {gpu, apis};
        OptionValues values;
        OptionsModel model(schema, &values);
        model.setSchema(schema);
        const QModelIndex row = model.index(1);
        QCOMPARE(model.data(row, OptionsModel::InfoValueRole).toString(), QStringLiteral("Vulkan 1.4.351"));
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.setValue(QStringLiteral("gpu"), QStringLiteral("amd"));
        QCOMPARE(model.data(row, OptionsModel::InfoValueRole).toString(), QStringLiteral("Vulkan 1.4.318 · N/A: DirectX is Windows only"));
        bool infoRowSignalled = false;
        for (const QList<QVariant> &args : changed)
            infoRowSignalled = infoRowSignalled || (args.at(0).value<QModelIndex>().row() == 1 && args.at(2).value<QList<int>>().contains(OptionsModel::InfoValueRole));
        QVERIFY(infoRowSignalled);
    }

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

        // A stub's options (Phase 3 N6: the real RAM test has info rows only).
        rc().toggle(QStringLiteral("storage"));
        QCOMPARE(rc().activeTab(), QStringLiteral("storage"));
        auto *options = qobject_cast<OptionsModel *>(rc().options());
        QVERIFY(options);
        QAbstractItemModelTester tester(options, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(options->rowCount() >= 6);  // sub-tests, drive, file size, folder, simulate, seed
        QCOMPARE(options->value(QStringLiteral("file_size")).toString(), QStringLiteral("1g"));
        QCOMPARE(options->value(QStringLiteral("subtests")).toStringList().size(), 6);
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
        // Storage's stub (Phase 3 N6: the RAM stub's duration option is gone).
        rc().toggle(QStringLiteral("storage"));
        const QString before = rc().estimateText();
        rc().setOption(QStringLiteral("storage"), QStringLiteral("file_size"), QStringLiteral("4g"));
        QVERIFY(rc().estimateText() != before);
        QCOMPARE(rc().valuesFor(QStringLiteral("storage")).value(QStringLiteral("file_size")).toString(), QStringLiteral("4g"));
        rc().setOption(QStringLiteral("storage"), QStringLiteral("seed"), 9999);
        QCOMPARE(rc().valuesFor(QStringLiteral("storage")).value(QStringLiteral("seed")).toDouble(), 9999.0);
        rc().setOption(QStringLiteral("storage"), QStringLiteral("seed"), 99999);  // clamped
        QCOMPARE(rc().valuesFor(QStringLiteral("storage")).value(QStringLiteral("seed")).toDouble(), 9999.0);
        rc().setOption(QStringLiteral("storage"), QStringLiteral("subtests"), QVariantList{QStringLiteral("latency")});
        QCOMPARE(rc().valuesFor(QStringLiteral("storage")).value(QStringLiteral("subtests")).toStringList(), QStringList{QStringLiteral("latency")});
        // An invalid choice is a problem that blocks Start, in words.
        rc().setOption(QStringLiteral("storage"), QStringLiteral("subtests"), QVariantList{});
        QVERIFY(!rc().canStart());
        QVERIFY(rc().problems().join(u' ').contains(QLatin1StringView("sub-test")));
    }

    void aRunFlowsThroughTheModelsAndLocksStorage()
    {
        startRuntime();
        rc().toggle(QStringLiteral("cpu"));
        rc().toggle(QStringLiteral("storage"));
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
        QVERIFY(log->toText(QStringLiteral("storage")).contains(QLatin1StringView("Storage")));
        QVERIFY(!log->toText(QStringLiteral("storage")).contains(QLatin1StringView("CPU")));
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
        rc().toggle(QStringLiteral("storage"));
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

    // Zeb: the ETA counts down inside a running test (it used to move only
    // when a test finished), and durations read "4 min 30 s" like the log.
    void durationWording()
    {
        QCOMPARE(RunController::durationText(0), QStringLiteral("1 s"));
        QCOMPARE(RunController::durationText(45), QStringLiteral("45 s"));
        QCOMPARE(RunController::durationText(89), QStringLiteral("89 s"));
        QCOMPARE(RunController::durationText(90), QStringLiteral("1 min 30 s"));
        QCOMPARE(RunController::durationText(270), QStringLiteral("4 min 30 s"));
        QCOMPARE(RunController::durationText(299), QStringLiteral("5 min"));
        QCOMPARE(RunController::durationText(3565), QStringLiteral("59 min 30 s"));
        QCOMPARE(RunController::durationText(3580), QStringLiteral("1 h 00 min"));
        QCOMPARE(RunController::durationText(3900), QStringLiteral("1 h 05 min"));
        QCOMPARE(RunController::durationText(7200), QStringLiteral("2 h 00 min"));
    }

    // Phase 4a P10, M8: while the interface is held, progress and log lines are
    // kept back (only the part shows), and applied in order on release.
    void heldInterfaceBuffersUpdates()
    {
        startRuntime();
        RunController &r = rc();
        QAbstractItemModel *log = r.log();
        const QString stageBefore = r.currentStage();
        const int rowsBefore = log->rowCount();
        QSignalSpy heldSpy(&r, &RunController::interfaceHeldChanged);
        QSignalSpy partSpy(&r, &RunController::heldPartChanged);
        QVERIFY(!r.interfaceHeld());
        r.orchestrator().interfaceHold(0, true);
        QVERIFY(r.interfaceHeld());
        QCOMPARE(heldSpy.count(), 1);

        const auto update = [&](const char *part, double fraction) {
            ProgressUpdate u;
            u.index = 0;
            u.testId = QStringLiteral("gpu");
            u.fraction = fraction;
            u.stage = QStringLiteral("stage");
            u.part = QString::fromUtf8(part);
            r.orchestrator().progress(u);
        };
        update("Geometry · Vulkan", 0.1);
        update("Geometry · Vulkan", 0.2);
        update("Geometry · DirectX 12", 0.3);
        for (const char *text : {"first", "second"}) {
            LogLine l;
            l.testId = QStringLiteral("gpu");
            l.message = QString::fromLatin1(text);
            r.orchestrator().logLine(l);
        }
        QCOMPARE(partSpy.count(), 2);
        QCOMPARE(r.heldPart(), QStringLiteral("Geometry · DirectX 12"));
        QCOMPARE(r.currentStage(), stageBefore);
        QCOMPARE(log->rowCount(), rowsBefore);

        r.orchestrator().interfaceHold(0, false);
        QVERIFY(!r.interfaceHeld());
        QCOMPARE(heldSpy.count(), 2);
        QCOMPARE(log->rowCount(), rowsBefore + 2);
        QCOMPARE(r.currentStage(), QStringLiteral("GPU · Geometry · DirectX 12"));  // the last update, applied once; the part never repeats the test name
        QVERIFY(r.heldPart().isEmpty());
        // Not held any more: updates apply at once again.
        update("", 0.5);
        QCOMPARE(r.currentStage(), QStringLiteral("GPU · stage"));
    }

    // AP-5 M1: a hung test's testFinished arrives before runFinished. The held
    // progress must not be replayed over the row's final "did not respond" text.
    void hungTestKeepsItsReasonAfterAHold()
    {
        startRuntime();
        RunController &r = rc();
        const QString reason = QStringLiteral("Did not respond, restart Syntheon");
        r.orchestrator().runStarted(RunRecord{}, QStringList{QStringLiteral("gpu")});
        r.orchestrator().testStarted(0, QStringLiteral("gpu"));
        r.orchestrator().interfaceHold(0, true);
        QVERIFY(r.interfaceHeld());
        QCOMPARE(r.heldTestName(), QStringLiteral("GPU"));
        ProgressUpdate u;
        u.index = 0;
        u.testId = QStringLiteral("gpu");
        u.fraction = 0.4;
        u.stage = QStringLiteral("stale stage");
        r.orchestrator().progress(u);
        TestResult hung = TestResult::withStatus(Status::Interrupted, reason);
        hung.testId = QStringLiteral("gpu");
        r.orchestrator().testFinished(0, hung);
        QVERIFY(!r.interfaceHeld());  // released by the test finishing
        const auto stage = [&] { return r.queue()->data(r.queue()->index(0, 0), RunQueueModel::StageRole).toString(); };
        QCOMPARE(stage(), reason);
        r.orchestrator().runFinished(RunRecord{});  // the safety net doesn't replay anything
        QCOMPARE(stage(), reason);
    }

    // AP-5 L3: a long hold keeps the newest lines, up to the log's own capacity.
    void heldLogIsCapped()
    {
        startRuntime();
        RunController &r = rc();
        QAbstractItemModel *log = r.log();
        const int before = log->rowCount();
        r.orchestrator().interfaceHold(0, true);
        for (int i = 0; i < 5200; ++i) {
            LogLine l;
            l.message = QString::number(i);
            r.orchestrator().logLine(l);
        }
        r.orchestrator().interfaceHold(0, false);
        QCOMPARE(log->rowCount(), std::min(5000, before + 5000));
        const QModelIndex last = log->index(log->rowCount() - 1, 0);
        QCOMPARE(log->data(last, LogModel::MessageRole).toString(), QStringLiteral("5199"));  // the newest kept
    }

    void etaCountsDownWithinATest()
    {
        tests::setStubTimeScale(1.0);
        startRuntime();
        rc().toggle(QStringLiteral("cpu"));
        QCOMPARE(rc().estimateText(), QStringLiteral("estimated 4 min 30 s"));
        QCOMPARE(rc().estimateOf(QStringLiteral("cpu")), QStringLiteral("~4 min 30 s"));
        rc().clearSelection();
        rc().toggle(QStringLiteral("ai"));  // a 30 s stub
        QSignalSpy finished(&rc(), &RunController::runFinished);
        QVERIFY(rc().start());
        QTRY_VERIFY_WITH_TIMEOUT(rc().runSubtitle().contains(QLatin1StringView("left")), 5000);
        const auto secondsLeft = [&] {
            const QString s = rc().runSubtitle();
            const qsizetype at = s.indexOf(QLatin1StringView("about "));
            return s.mid(at + 6).section(QLatin1Char(' '), 0, 0).toInt();
        };
        const int first = secondsLeft();
        QTest::qWait(3200);
        const int later = secondsLeft();
        QVERIFY2(later < first, qPrintable(QStringLiteral("%1 -> %2").arg(first).arg(later)));
        rc().stop();
        QVERIFY(finished.wait(20000));
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
