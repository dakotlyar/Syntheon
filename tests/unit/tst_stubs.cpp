// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/orchestrator.h"
#include "core/power_inhibitor.h"
#include "core/registry.h"
#include "core/units.h"
#include "fake_test.h"
#include "storage/database.h"
#include "storage/recovery.h"
#include "storage/run_repository.h"
#include "storage/sqlite_run_sink.h"
#include "tests/builtin_tests.h"
#include "tests/common/stub_clock.h"

#include <QCborMap>
#include <QElapsedTimer>
#include <QCborValue>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace sb;
using namespace sb::testing;
using namespace std::chrono_literals;

namespace {

class NoInhibitor : public PowerInhibitor
{
public:
    bool acquire(const QString &, QString *) override { return true; }
    void release() override {}
};

SystemSnapshot batterySnapshot(bool present, bool ac, double charge)
{
    SystemSnapshot s;
    s.data.insert(QStringLiteral("battery"),
                  QJsonObject{{QStringLiteral("present"), present}, {QStringLiteral("ac_online"), ac},
                              {QStringLiteral("charge_pct"), charge}, {QStringLiteral("health_pct"), 90.0},
                              {QStringLiteral("design_wh"), 70.0}});
    return s;
}

Orchestrator::Config fastConfig()
{
    Orchestrator::Config c;
    c.grace = 400ms;
    c.watchdogInterval = 10ms;
    c.logFlushInterval = 30ms;
    return c;
}

} // namespace

class TestStubs : public QObject
{
    Q_OBJECT

private:
    Registry m_registry;
    NoInhibitor m_inhibitor;
    QTemporaryDir m_dir;
    std::unique_ptr<SqliteRunSink> m_sink;

    RunRecord runOne(const QString &id, OptionValues options = {}, RunMeta meta = {})
    {
        Orchestrator o(m_registry, *m_sink, m_inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        const auto r = o.start({{id, options}}, meta);
        [&] { QVERIFY2(r.ok, qPrintable(r.errors.join(u'\n'))); }();
        if (!r.ok)
            return {};
        [&] { QVERIFY(finished.wait(60000)); }();
        return finished.isEmpty() ? RunRecord{} : finished.last().at(0).value<RunRecord>();
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<RunRecord>();
        qRegisterMetaType<TestResult>();
        qRegisterMetaType<ProgressUpdate>();
        qRegisterMetaType<LogLine>();
        tests::setStubTimeScale(0.02);
        registerBuiltinTests(m_registry);
        QVERIFY(Database::migrate(m_dir.filePath(QStringLiteral("synbench.db"))).ok());
        m_sink = std::make_unique<SqliteRunSink>(m_dir.filePath(QStringLiteral("synbench.db")));
    }

    void cleanupTestCase()
    {
        m_sink.reset();
        tests::setStubTimeScale(0);
    }

    // Defaults are valid and every option kind is used somewhere.
    void optionSchemasCoverEveryKind()
    {
        FakeProbeContext ctx;
        QSet<int> kinds;
        for (const auto &t : m_registry.tests()) {
            if (t->info().id == QLatin1StringView("cpu"))
                continue;  // the real CPU test (Phase 2), not a stub
            const Availability a = t->probe(ctx);
            const OptionSchema s = effectiveOptionSchema(*t, ctx, a);
            if (a.state == Availability::State::Unavailable) {
                QVERIFY(s.options.isEmpty());
                continue;
            }
            const Validation v = validateOptions(s, s.defaults());
            QVERIFY2(v.ok(), qPrintable(t->info().id + QStringLiteral(": ") + (v.ok() ? QString() : v.errors.first().message)));
            QVERIFY(t->validate(s.defaults()).ok());
            QVERIFY(s.find(QStringLiteral("simulate")));
            QVERIFY(s.find(QStringLiteral("seed")));
            for (const OptionDef &o : s.options)
                kinds.insert(int(o.kind));
        }
        for (OptionKind k : {OptionKind::Bool, OptionKind::MultiSelect, OptionKind::SingleSelect, OptionKind::DynamicSelect,
                             OptionKind::Number, OptionKind::Path, OptionKind::Info})
            QVERIFY2(kinds.contains(int(k)), qPrintable(QString::number(int(k))));
        // Both SingleSelect presentations and a disabled item with a reason.
        QCOMPARE(m_registry.find(QStringLiteral("battery"))->options(ctx, {}).find(QStringLiteral("profile"))->presentation,
                 SelectPresentation::Segmented);
        QCOMPARE(m_registry.find(QStringLiteral("ram"))->options(ctx, {}).find(QStringLiteral("buffer"))->presentation,
                 SelectPresentation::Dropdown);
        const auto ai = m_registry.find(QStringLiteral("ai"))->options(ctx, {});
        QVERIFY(!ai.find(QStringLiteral("runtimes"))->item(QStringLiteral("openvino"))->enabled);
    }

    void npuIsNotAvailable()
    {
        FakeProbeContext ctx;
        const Availability a = m_registry.find(QStringLiteral("npu"))->probe(ctx);
        QCOMPARE(a.state, Availability::State::Unavailable);
        QVERIFY(a.reason.contains(QLatin1StringView("No supported NPU")));
        ctx.snapshot.data.insert(QStringLiteral("npus"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("AMD XDNA NPU")}}});
        QVERIFY(m_registry.find(QStringLiteral("npu"))->probe(ctx).reason.contains(QLatin1StringView("AMD XDNA NPU")));
        const TestResult na = TestResult::notAvailable(QStringLiteral("x"));
        const DetailLayout d = m_registry.find(QStringLiteral("npu"))->detail(na);
        QVERIFY(std::holds_alternative<NaStateBlock>(d.blocks.first().content));
    }

    void everyStubRunsAndScoresNearItsTarget()
    {
        const QHash<QString, double> targets = {{QStringLiteral("ram"), 612},
                                                {QStringLiteral("storage"), 938}, {QStringLiteral("gpu"), 1206},
                                                {QStringLiteral("ai"), 772}};
        for (const QString &id : targets.keys()) {
            const RunRecord run = runOne(id);
            QCOMPARE(run.status, RunStatus::Completed);
            const TestResult &t = run.tests.first();
            QVERIFY2(t.status == Status::Ok, qPrintable(id + QStringLiteral(" ") + t.statusReason));
            QVERIFY(t.score);
            const double target = targets[id];
            QVERIFY2(std::abs(*t.score - target) < target * 0.08,
                     qPrintable(QStringLiteral("%1 scored %2, target %3").arg(id).arg(*t.score).arg(target)));
            QVERIFY(!t.metrics.isEmpty());
            for (const Metric &m : t.metrics) {
                if (m.isNumeric())
                    QVERIFY(std::isfinite(std::get<double>(m.value)));
                if (!m.unit.isEmpty())
                    QVERIFY2(Units::isCanonical(m.unit), qPrintable(m.unit));
            }
            // Sub-scores add up to the total, apart from metrics that belong to
            // no sub-test.
            double sum = 0;
            for (const SubResult &s : t.subResults) {
                QCOMPARE(s.status, Status::Ok);
                if (s.score)
                    sum += *s.score;
            }
            QVERIFY(sum <= *t.score + t.subResults.size());
            QVERIFY2(sum >= *t.score * 0.9, qPrintable(id));
            // The detail layout renders from the stored data.
            const DetailLayout d = m_registry.find(id)->detail(t);
            QVERIFY(!d.blocks.isEmpty());
            QVERIFY(d.subtitle.startsWith(QLatin1StringView("Score ")));
            QVERIFY(!d.headlineMetrics.isEmpty());
            for (const QString &k : d.headlineMetrics)
                QVERIFY2(t.metric(k), qPrintable(id + QStringLiteral(" headline ") + k));
        }
    }

    void seededVariation()
    {
        const QVariantMap seed1{{QStringLiteral("seed"), 1.0}}, seed2{{QStringLiteral("seed"), 2.0}};
        const RunRecord a = runOne(QStringLiteral("gpu"), seed1), b = runOne(QStringLiteral("gpu"), seed1);
        const RunRecord c = runOne(QStringLiteral("gpu"), seed2);
        QCOMPARE(a.tests[0].metrics, b.tests[0].metrics);  // same seed, same numbers
        QCOMPARE(a.tests[0].score, b.tests[0].score);
        QVERIFY(a.tests[0].metrics != c.tests[0].metrics);
        // ±5 % around the example value.
        const double v = std::get<double>(a.tests[0].metric(QStringLiteral("native2d.shapes"))->value);
        QVERIFY(v > 2.94 * 0.95 && v < 2.94 * 1.05);
    }

    void subTestSelection()
    {
        const RunRecord run = runOne(QStringLiteral("gpu"), {{QStringLiteral("subtests"), QStringList{QStringLiteral("fill2d"), QStringLiteral("scenes3d")}}});
        const TestResult &t = run.tests[0];
        QCOMPARE(t.subResults.size(), 2);
        QCOMPARE(t.subResults[0].id, QStringLiteral("scenes3d"));  // declaration order
        QVERIFY(t.metric(QStringLiteral("fill2d.opengl")));
        QVERIFY(!t.metric(QStringLiteral("native2d.shapes")));
        QVERIFY(t.score);
        // Nothing selected is rejected before the run starts.
        Orchestrator o(m_registry, *m_sink, m_inhibitor, fastConfig());
        QVERIFY(!o.start({{QStringLiteral("gpu"), {{QStringLiteral("subtests"), QStringList{}}}}}, {}).ok);
    }

    void gpuAndAiFollowTheirSelections()
    {
        const RunRecord gpu = runOne(QStringLiteral("gpu"), {{QStringLiteral("apis"), QStringList{QStringLiteral("opengl")}}});
        QVERIFY(gpu.tests[0].metric(QStringLiteral("scenes3d.opengl.fps")));
        QVERIFY(!gpu.tests[0].metric(QStringLiteral("scenes3d.vulkan.fps")));
        const RunRecord ai = runOne(QStringLiteral("ai"), {{QStringLiteral("runtimes"), QStringList{QStringLiteral("tensorrt")}},
                                                           {QStringLiteral("subtests"), QStringList{QStringLiteral("llm")}}});
        QVERIFY(ai.tests[0].metric(QStringLiteral("llm.tensorrt")));
        // UTF-8 names reach the labels intact (Zeb: "Ã— real time").
        const RunRecord speech = runOne(QStringLiteral("ai"), {{QStringLiteral("subtests"), QStringList{QStringLiteral("speech")}}});
        bool times = false;
        for (const Metric &m : speech.tests[0].metrics)
            times = times || m.label.contains(QStringLiteral("× real time"));
        QVERIFY(times);
        QVERIFY(!ai.tests[0].metric(QStringLiteral("llm.llamacpp")));
        const DetailLayout d = m_registry.find(QStringLiteral("ai"))->detail(ai.tests[0]);
        const auto *table = std::get_if<TableBlock>(&d.blocks.first().content);
        QVERIFY(table);
        QCOMPARE(table->columns, (QStringList{QStringLiteral("Workload"), QStringLiteral("TensorRT")}));
    }

    void storageGridAndRamChart()
    {
        const TestResult sto = runOne(QStringLiteral("storage")).tests[0];
        const DetailLayout d = m_registry.find(QStringLiteral("storage"))->detail(sto);
        const auto *grid = std::get_if<GridBlock>(&d.blocks.first().content);
        QVERIFY(grid);
        QCOMPARE(grid->rowLabels.size(), 4);
        QCOMPARE(grid->cells[2][0].secondaryMetricKey, QStringLiteral("rnd4k_q32t1.read_iops"));
        QVERIFY(grid->cells[0][0].secondaryMetricKey.isEmpty());

        const TestResult ram = runOne(QStringLiteral("ram")).tests[0];
        QCOMPARE(ram.series.size(), 1);
        QCOMPARE(ram.series[0].points.size(), 17);
        QVERIFY(ram.series[0].points.first().y() < 2 && ram.series[0].points.last().y() > 60);
        bool chart = false;
        for (const DetailBlock &b : m_registry.find(QStringLiteral("ram"))->detail(ram).blocks)
            chart = chart || std::holds_alternative<LineChartBlock>(b.content);
        QVERIFY(chart);
    }

    void simulateModes()
    {
        const RunRecord err = runOne(QStringLiteral("gpu"), {{QStringLiteral("simulate"), QStringLiteral("error")}});
        QCOMPARE(err.tests[0].status, Status::Error);
        QVERIFY(err.tests[0].statusReason.startsWith(QLatin1StringView("Simulated error")));
        QVERIFY(!err.tests[0].score);
        QCOMPARE(err.status, RunStatus::Completed);  // an error doesn't stop the run

        const RunRecord na = runOne(QStringLiteral("gpu"), {{QStringLiteral("simulate"), QStringLiteral("na")}});
        QCOMPARE(na.tests[0].status, Status::NotAvailable);

        // Hang ignores Stop and the time limit: the run is abandoned (R7). The
        // limit floor (normally 15 s so slow CI never times a stub out) is
        // lowered so this test is quick.
        tests::setStubTimeLimitFloor(std::chrono::seconds(1));
        Orchestrator o(m_registry, *m_sink, m_inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start({{QStringLiteral("ram"), {{QStringLiteral("simulate"), QStringLiteral("hang")}}}}, {}).ok);
        QVERIFY(finished.wait(30000));
        const RunRecord run = finished.last().at(0).value<RunRecord>();
        QCOMPARE(run.status, RunStatus::Interrupted);
        QCOMPARE(run.tests[0].statusReason, QStringLiteral("Did not respond, restart SynBench"));
        QCOMPARE(o.state(), Orchestrator::State::Abandoned);
        tests::releaseStubHangs();  // let the hung stub finish so it can be joined
        QVERIFY(o.waitForWorkerExit(10s));
        tests::resetStubTimeLimitFloor();

        // Unknown modes are rejected up front.
        Orchestrator o2(m_registry, *m_sink, m_inhibitor, fastConfig());
        QVERIFY(!o2.start({{QStringLiteral("gpu"), {{QStringLiteral("simulate"), QStringLiteral("explode")}}}}, {}).ok);
    }

    // AP-5 #12: odd stub-scale text and seed values are handled, not undefined.
    void scaleAndSeedAreSanitised()
    {
        QCOMPARE(tests::parseStubScale(QStringLiteral("0.05")), std::optional<double>(0.05));
        for (const char *bad : {"inf", "-inf", "nan", "1e308", "1e400", "-1", "0", "", "abc", "100.5"})
            QVERIFY2(!tests::parseStubScale(QString::fromLatin1(bad)), bad);
        QCOMPARE(tests::parseStubScale(QStringLiteral("100")), std::optional<double>(100.0));
        // A seed from the UI can be anything a number box lets through.
        tests::setStubTimeScale(0.02);
        for (double seed : {-5.0, 1e300, 4294967296.0 * 3})
            QCOMPARE(runOne(QStringLiteral("gpu"), {{QStringLiteral("seed"), seed}}).tests[0].status, Status::Ok);
        tests::setStubTimeScale(0.02);
    }

    // AP-5 #12: a Stop that arrives during the last slice is Cancelled, never
    // Ok. Deterministic: the context turns "cancelled" on once the stub has
    // reported 100% progress, i.e. exactly in its final slice.
    void stopInTheFinalSliceIsNotOk()
    {
        struct LastSliceContext : RunContext {
            bool stop = false;
            SystemSnapshot snap;
            UnavailablePrivilegedOps ops;
            void progress(double fraction, const QString &) override { stop = stop || fraction >= 0.999; }
            void log(LogLevel, const QString &) override {}
            bool cancelled() const override { return stop; }
            void subTestBegin(const QString &) override {}
            void subTestEnd(const QString &, Status) override {}
            void checkpoint(const QCborMap &) override {}
            QString scratchDir() override { return {}; }
            const SystemSnapshot &system() const override { return snap; }
            PrivilegedOps &privileged() override { return ops; }
        };
        tests::setStubTimeScale(0.02);
        for (const QString &id : {QStringLiteral("gpu"), QStringLiteral("ram"), QStringLiteral("battery")}) {
            LastSliceContext ctx;
            OptionValues o;
            if (id == QLatin1StringView("battery"))
                o.insert(QStringLiteral("override_preconditions"), true);
            const TestResult r = m_registry.find(id)->run(ctx, o);
            QCOMPARE(r.status, Status::Cancelled);
        }
    }

    // AP-5 #12: a hung Battery stub (no time limit otherwise) has a way out.
    void hungBatteryStubIsAbandonedByItsTimeLimit()
    {
        const ITest *battery = m_registry.find(QStringLiteral("battery"));
        QCOMPARE(battery->timeLimit({}), std::chrono::seconds(0));
        QVERIFY(battery->timeLimit({{QStringLiteral("simulate"), QStringLiteral("hang")}}) > std::chrono::seconds(0));
        tests::setStubTimeLimitFloor(std::chrono::seconds(1));
        Orchestrator o(m_registry, *m_sink, m_inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start({{QStringLiteral("battery"), {{QStringLiteral("simulate"), QStringLiteral("hang")},
                                                      {QStringLiteral("override_preconditions"), true}}}}, {}).ok);
        QVERIFY(finished.wait(30000));
        QCOMPARE(o.state(), Orchestrator::State::Abandoned);
        tests::releaseStubHangs();
        QVERIFY(o.waitForWorkerExit(10s));
        tests::resetStubTimeLimitFloor();
    }

    // A slow machine must never turn a healthy stub into a timeout: the time
    // limit has a floor that doesn't shrink with the time scale.
    void timeLimitHasAFloorBelowRealTime()
    {
        const ITest *gpu = m_registry.find(QStringLiteral("gpu"));
        tests::setStubTimeScale(0.03);
        QVERIFY(gpu->timeLimit({}) >= std::chrono::seconds(15));
        tests::setStubTimeScale(1.0);
        QCOMPARE(gpu->timeLimit({}), std::chrono::seconds(48));  // 2 x 24 s, as before
        tests::setStubTimeLimitFloor(std::chrono::seconds(3));
        tests::setStubTimeScale(0.03);
        QCOMPARE(gpu->timeLimit({}), std::chrono::seconds(3));
        tests::resetStubTimeLimitFloor();
        QCOMPARE(m_registry.find(QStringLiteral("battery"))->timeLimit({}), std::chrono::seconds(0));
        tests::setStubTimeScale(0.02);
    }

    // The stubs take about their nominal time, whatever the sleep granularity.
    void stubsRunInAboutTheirNominalTime()
    {
        QElapsedTimer t;
        t.start();
        const RunRecord run = runOne(QStringLiteral("gpu"));
        const qint64 ms = t.elapsed();
        QCOMPARE(run.tests[0].status, Status::Ok);
        QVERIFY2(ms >= 24 * 20 - 30 && ms < 24 * 20 + 1500, qPrintable(QString::number(ms)));  // 24 s x 0.02 = 480 ms
    }

    void stopCancelsAStub()
    {
        tests::setStubTimeScale(1.0);  // long enough to stop
        Orchestrator o(m_registry, *m_sink, m_inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QSignalSpy started(&o, &Orchestrator::testStarted);
        QVERIFY(o.start({{QStringLiteral("gpu"), {}}, {QStringLiteral("ram"), {}}}, {}).ok);
        QVERIFY(started.wait(5000));
        QTest::qWait(200);
        o.stop();
        QVERIFY(finished.wait(10000));
        const RunRecord run = finished.last().at(0).value<RunRecord>();
        QCOMPARE(run.status, RunStatus::Stopped);
        QCOMPARE(run.tests[0].status, Status::Cancelled);
        QCOMPARE(run.tests[1].status, Status::NotRun);
        tests::setStubTimeScale(0.02);
    }

    void batteryPreconditions()
    {
        const ITest *bat = m_registry.find(QStringLiteral("battery"));
        FakeProbeContext ctx;
        const auto states = [&](const SystemSnapshot &s, bool override) {
            ctx.snapshot = s;
            QMap<QString, Precondition::State> out;
            for (const Precondition &p : bat->preconditions({{QStringLiteral("override_preconditions"), override}}, ctx))
                out.insert(p.id, p.state);
            return out;
        };
        auto laptopOnAc = states(batterySnapshot(true, true, 100), false);
        QCOMPARE(laptopOnAc[QStringLiteral("unplugged")], Precondition::State::NotMet);
        QCOMPARE(laptopOnAc[QStringLiteral("charged")], Precondition::State::Met);
        QCOMPARE(laptopOnAc[QStringLiteral("brightness")], Precondition::State::WillApplyAtStart);
        auto ready = states(batterySnapshot(true, false, 100), false);
        QCOMPARE(ready[QStringLiteral("unplugged")], Precondition::State::Met);
        QCOMPARE(ready[QStringLiteral("battery")], Precondition::State::Met);
        QCOMPARE(states(batterySnapshot(true, false, 80), false)[QStringLiteral("charged")], Precondition::State::NotMet);
        // A desktop: no battery. The override clears every NotMet.
        auto desktop = states(batterySnapshot(false, true, -1), false);
        QCOMPARE(desktop[QStringLiteral("battery")], Precondition::State::NotMet);
        auto overridden = states(batterySnapshot(false, true, -1), true);
        for (auto it = overridden.cbegin(); it != overridden.cend(); ++it)
            QVERIFY(it.value() != Precondition::State::NotMet);

        // The orchestrator refuses to start on NotMet and allows the override.
        Orchestrator o(m_registry, *m_sink, m_inhibitor, fastConfig());
        RunMeta meta;
        meta.system = batterySnapshot(false, true, -1);
        const auto refused = o.start({{QStringLiteral("battery"), {}}}, meta);
        QVERIFY(!refused.ok);
        QVERIFY(refused.errors.join(u' ').contains(QLatin1StringView("No battery found")));
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start({{QStringLiteral("battery"), {{QStringLiteral("override_preconditions"), true}}}}, meta).ok);
        QVERIFY(finished.wait(30000));
    }

    void batteryRunCheckpointsAndRecovers()
    {
        RunMeta meta;
        meta.system = batterySnapshot(true, false, 100);
        const RunRecord run = runOne(QStringLiteral("battery"), {{QStringLiteral("profile"), QStringLiteral("light")}}, meta);
        QCOMPARE(run.kind, RunKind::Exclusive);
        const TestResult &t = run.tests[0];
        QCOMPARE(t.status, Status::Ok);
        const double minutes = std::get<double>(t.metric(QStringLiteral("runtime"))->value);
        QVERIFY(minutes > 560 * 0.95 && minutes < 560 * 1.05);
        QCOMPARE(t.series[0].points.size(), 25);
        QVERIFY(t.series[0].points.last().y() < 5);
        QVERIFY(t.score);
        QVERIFY(!run.overall);  // Battery's overall contribution is a Phase 8 decision

        // The checkpoints reached the database.
        Database::ScopedConnection c(m_dir.filePath(QStringLiteral("synbench.db")), QStringLiteral("t"));
        RunRepository repo(c.db());
        QCOMPARE(repo.loadCheckpoints(*repo.runId(run.uuid), QStringLiteral("battery")).size(), 25);

        // A run cut short is recovered from them.
        RunRecord cut;
        cut.uuid = QUuid::createUuid();
        cut.kind = RunKind::Exclusive;
        cut.status = RunStatus::Running;
        cut.startedAt = QDateTime::currentDateTimeUtc().addSecs(-60);
        cut.system = batterySnapshot(true, false, 100);
        TestResult placeholder = TestResult::withStatus(Status::NotRun, QStringLiteral("Queued"));
        placeholder.testId = QStringLiteral("battery");
        placeholder.options = {{QStringLiteral("profile"), QStringLiteral("mixed")}};
        cut.tests = {placeholder};
        const qint64 id = *repo.saveRun(cut);
        for (int n = 0; n < 6; ++n)
            repo.addCheckpoint(id, QStringLiteral("battery"), n,
                               QCborValue(QCborMap{{QStringLiteral("t"), n * 15.0}, {QStringLiteral("charge"), 100.0 - n * 4}}).toCbor());
        const RecoveryReport report = recoverRuns(c.db(), m_registry);
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(report.notices.first().status, RunStatus::Recovered);
        const RunRecord rec = *repo.load(cut.uuid);
        QCOMPARE(rec.status, RunStatus::Recovered);
        QCOMPARE(rec.tests[0].status, Status::Ok);
        QCOMPARE(std::get<double>(rec.tests[0].metric(QStringLiteral("runtime"))->value), 75.0);
        QCOMPARE(rec.tests[0].statusReason, QStringLiteral("Recovered from checkpoints"));
        QCOMPARE(rec.tests[0].series[0].points.size(), 6);
        // No checkpoints at all: interrupted, not recovered.
        RunRecord none = cut;
        none.uuid = QUuid::createUuid();
        repo.saveRun(none);
        recoverRuns(c.db(), m_registry);
        QCOMPARE(repo.load(none.uuid)->status, RunStatus::Interrupted);
    }
};

QTEST_GUILESS_MAIN(TestStubs)
#include "tst_stubs.moc"
