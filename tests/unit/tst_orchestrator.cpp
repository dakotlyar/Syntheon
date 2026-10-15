// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/orchestrator.h"
#include "core/power_inhibitor.h"
#include "core/registry.h"
#include "core/run_sink.h"
#include "fake_test.h"
#include "storage/database.h"
#include "storage/run_repository.h"
#include "storage/sqlite_run_sink.h"

#include <QCborValue>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <stdexcept>
#include <thread>

using namespace sb;
using namespace sb::testing;
using namespace std::chrono_literals;

namespace {

// Ordered, thread-tagged event log shared by the recording sink and inhibitor.
struct Events {
    struct Event {
        QString what;
        std::thread::id thread;
    };
    void add(const QString &what)
    {
        QMutexLocker l(&mutex);
        list.append({what, std::this_thread::get_id()});
    }
    QStringList names() const
    {
        QMutexLocker l(&mutex);
        QStringList out;
        for (const auto &e : list)
            out.append(e.what);
        return out;
    }
    int indexOf(const QString &what) const { return names().indexOf(what); }
    int size() const
    {
        QMutexLocker l(&mutex);
        return list.size();
    }
    std::thread::id threadOf(const QString &what) const
    {
        QMutexLocker l(&mutex);
        for (const auto &e : list) {
            if (e.what == what)
                return e.thread;
        }
        return {};
    }
    mutable QMutex mutex;
    QVector<Event> list;
};

// Records every call; optionally forwards to a real SqliteRunSink.
class RecordingSink : public RunSink
{
public:
    explicit RecordingSink(Events &events, RunSink *forward = nullptr) : m_events(events), m_forward(forward) {}

    bool beginRun(const RunRecord &run, QString *error) override
    {
        m_events.add(QStringLiteral("beginRun"));
        {
            QMutexLocker l(&mutex);
            begun = run;
        }
        if (failBegin) {
            *error = QStringLiteral("disk full");
            return false;
        }
        return !m_forward || m_forward->beginRun(run, error);
    }
    bool commitTest(const QUuid &run, int position, const TestResult &r, QString *error) override
    {
        m_events.add(QStringLiteral("commit:%1:%2").arg(position).arg(r.testId));
        {
            QMutexLocker l(&mutex);
            commits.append({position, r});
        }
        return !m_forward || m_forward->commitTest(run, position, r, error);
    }
    bool appendLog(const QUuid &run, const QVector<LogLine> &lines, QString *error) override
    {
        m_events.add(QStringLiteral("appendLog"));
        {
            QMutexLocker l(&mutex);
            logLines += lines;
            ++logBatches;
        }
        return !m_forward || m_forward->appendLog(run, lines, error);
    }
    bool finishRun(const RunRecord &run, QString *error) override
    {
        m_events.add(QStringLiteral("finishRun"));
        {
            QMutexLocker l(&mutex);
            finished = run;
        }
        return !m_forward || m_forward->finishRun(run, error);
    }
    bool addCheckpoint(const QUuid &run, const QString &testId, int seq, const QByteArray &cbor,
                       QString *error) override
    {
        m_events.add(QStringLiteral("checkpoint:%1:%2").arg(testId).arg(seq));
        {
            QMutexLocker l(&mutex);
            checkpoints.append(cbor);
        }
        return !m_forward || m_forward->addCheckpoint(run, testId, seq, cbor, error);
    }
    bool addScratchPath(const QUuid &run, const QString &path, QString *error) override
    {
        m_events.add(QStringLiteral("addScratch"));
        return !m_forward || m_forward->addScratchPath(run, path, error);
    }
    bool removeScratchPath(const QUuid &run, const QString &path, QString *error) override
    {
        m_events.add(QStringLiteral("removeScratch"));
        return !m_forward || m_forward->removeScratchPath(run, path, error);
    }
    void setDurable(bool full) override
    {
        m_events.add(full ? QStringLiteral("durable:on") : QStringLiteral("durable:off"));
        if (m_forward)
            m_forward->setDurable(full);
    }
    void threadFinished() override
    {
        m_events.add(QStringLiteral("threadFinished"));
        if (m_forward)
            m_forward->threadFinished();
    }

    QMutex mutex;
    RunRecord begun, finished;
    QVector<std::pair<int, TestResult>> commits;
    QVector<LogLine> logLines;
    int logBatches = 0;
    QVector<QByteArray> checkpoints;
    bool failBegin = false;

private:
    Events &m_events;
    RunSink *m_forward;
};

class RecordingInhibitor : public PowerInhibitor
{
public:
    explicit RecordingInhibitor(Events &events) : m_events(events) {}
    bool acquire(const QString &, QString *error) override
    {
        m_events.add(QStringLiteral("acquire"));
        if (fail) {
            *error = QStringLiteral("no logind");
            return false;
        }
        held = true;
        return true;
    }
    void release() override
    {
        m_events.add(QStringLiteral("release"));
        held = false;
    }
    std::atomic<bool> held{false};
    bool fail = false;

private:
    Events &m_events;
};

TestResult okResult(double value)
{
    TestResult r;
    r.status = Status::Ok;
    r.metrics = {{QStringLiteral("value"), {}, QStringLiteral("Value"), value, QStringLiteral("points"),
                  Direction::HigherIsBetter, true}};
    return r;
}

ScoreBreakdown scoreFromValue(const TestResult &r)
{
    ScoreBreakdown b;
    if (const Metric *m = r.metric(QStringLiteral("value")))
        b.total = std::get<double>(m->value);
    return b;
}

// Runs until cancelled, reporting progress; returns Cancelled.
TestResult untilCancelled(RunContext &ctx, const OptionValues &)
{
    for (int i = 0; !ctx.cancelled(); ++i) {
        ctx.progress((i % 100) / 100.0, QStringLiteral("Working"));
        QThread::msleep(5);
    }
    return TestResult::withStatus(Status::Cancelled);
}

RunMeta meta()
{
    RunMeta m;
    m.appVersion = QStringLiteral("2026.11.0");
    m.appBuild = QStringLiteral("linux-x64 test");
    m.system.machineLabel = QStringLiteral("Test machine");
    return m;
}

Orchestrator::Config fastConfig()
{
    Orchestrator::Config c;
    c.grace = 300ms;
    c.watchdogInterval = 10ms;
    c.logFlushInterval = 30ms;
    c.progressInterval = 20ms;
    return c;
}

// cpu, ram, gpu (shared) and battery (runs alone, recoverable), in that
// canonical order. Each fake returns Ok with value 100/200/300 by default.
struct Fixture {
    Fixture()
    {
        const QStringList ids = {QStringLiteral("cpu"), QStringLiteral("ram"), QStringLiteral("gpu")};
        double v = 100;
        for (const QString &id : ids) {
            auto t = std::make_unique<FakeTest>(FakeTest::makeInfo(id));
            const double value = v;
            t->runner = [value](RunContext &, const OptionValues &) { return okResult(value); };
            t->scorer = scoreFromValue;
            fakes.insert(id, t.get());
            registry.add(std::move(t));
            v += 100;
        }
        TestInfo bi = FakeTest::makeInfo(QStringLiteral("battery"), Exclusivity::RunsAlone);
        bi.recovery = RecoveryMode::FromCheckpoints;
        bi.countsTowardOverall = false;
        auto b = std::make_unique<FakeTest>(bi);
        b->limit = 0s;
        b->runner = [](RunContext &, const OptionValues &) { return okResult(42); };
        b->scorer = scoreFromValue;
        fakes.insert(QStringLiteral("battery"), b.get());
        registry.add(std::move(b));
    }
    FakeTest *operator[](const QString &id) { return fakes.value(id); }

    Registry registry;
    QHash<QString, FakeTest *> fakes;
    Events events;
    RecordingInhibitor inhibitor{events};
};

Plan plan(const QStringList &ids)
{
    Plan p;
    for (const QString &id : ids)
        p.append({id, {}});
    return p;
}

bool waitFinished(QSignalSpy &spy, int timeoutMs = 15000)
{
    return spy.count() > 0 || spy.wait(timeoutMs);
}

RunRecord finishedRun(QSignalSpy &spy)
{
    return spy.last().at(0).value<RunRecord>();
}

QStringList statuses(const RunRecord &r)
{
    QStringList out;
    for (const TestResult &t : r.tests)
        out.append(t.testId + QLatin1Char('=') + toKey(t.status));
    return out;
}

} // namespace

class TestOrchestrator : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<RunRecord>();
        qRegisterMetaType<TestResult>();
        qRegisterMetaType<ProgressUpdate>();
        qRegisterMetaType<LogLine>();
    }

    void canonicalOrderAndScores()
    {
        Fixture f;
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy started(&o, &Orchestrator::runStarted);
        QSignalSpy finished(&o, &Orchestrator::runFinished);

        const auto r = o.start(plan({QStringLiteral("gpu"), QStringLiteral("cpu"), QStringLiteral("ram")}), meta());
        QVERIFY2(r.ok, qPrintable(r.errors.join(u'\n')));
        QCOMPARE(o.state(), Orchestrator::State::Running);
        QCOMPARE(started.count(), 1);
        QCOMPARE(started.first().at(1).toStringList(),
                 (QStringList{QStringLiteral("cpu"), QStringLiteral("ram"), QStringLiteral("gpu")}));
        QVERIFY(waitFinished(finished));
        QCOMPARE(o.state(), Orchestrator::State::Idle);

        const RunRecord run = finishedRun(finished);
        QCOMPARE(run.uuid, r.runId);
        QCOMPARE(run.kind, RunKind::Standard);
        QCOMPARE(run.status, RunStatus::Completed);
        QCOMPARE(statuses(run), (QStringList{QStringLiteral("cpu=ok"), QStringLiteral("ram=ok"),
                                             QStringLiteral("gpu=ok")}));
        QCOMPARE(run.tests[0].score, std::optional<double>(100));
        QCOMPARE(run.tests[2].score, std::optional<double>(300));
        QCOMPARE(run.overall, std::optional<double>(600));
        QVERIFY(run.finishedAt.isValid());
        QVERIFY(run.tests[0].startedAt.isValid());
        QCOMPARE(run.appVersion, QStringLiteral("2026.11.0"));
        QCOMPARE(run.system.machineLabel, QStringLiteral("Test machine"));
        QCOMPARE(o.currentRun(), run);

        // Positions follow canonical order.
        QCOMPARE(sink.commits.size(), 3);
        QCOMPARE(sink.commits[0].first, 0);
        QCOMPARE(sink.commits[0].second.testId, QStringLiteral("cpu"));
        QCOMPARE(sink.commits[2].second.testId, QStringLiteral("gpu"));
        QCOMPARE(sink.finished, run);
    }

    void progressThrottledAndSubTests()
    {
        Fixture f;
        f[QStringLiteral("cpu")]->runner = [](RunContext &ctx, const OptionValues &) {
            ctx.subTest(QStringLiteral("int_fp")).begin();
            for (int i = 0; i < 2000; ++i)
                ctx.progress(i / 2000.0, QStringLiteral("Integer"));
            ctx.progress(1.0, QStringLiteral("Integer"));
            ctx.subTest(QStringLiteral("int_fp")).end(Status::Ok);
            return okResult(1);
        };
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy progress(&o, &Orchestrator::progress);
        QSignalSpy sub(&o, &Orchestrator::subTestChanged);
        QSignalSpy testStarted(&o, &Orchestrator::testStarted);
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("cpu")}), meta()).ok);
        QVERIFY(waitFinished(finished));

        QCOMPARE(testStarted.count(), 1);
        QVERIFY(progress.count() >= 2);
        QVERIFY2(progress.count() < 100, qPrintable(QString::number(progress.count())));  // throttled
        const auto last = progress.last().at(0).value<ProgressUpdate>();
        QCOMPARE(last.fraction, 1.0);
        QCOMPARE(last.stage, QStringLiteral("Integer"));
        QCOMPARE(last.index, 0);
        QCOMPARE(sub.count(), 2);
        QCOMPARE(sub.at(0).at(2).toBool(), false);
        QCOMPARE(sub.at(1).at(2).toBool(), true);
    }

    void logStreamedAndSavedInBatches()
    {
        Fixture f;
        f[QStringLiteral("cpu")]->runner = [](RunContext &ctx, const OptionValues &) {
            for (int i = 0; i < 20; ++i) {
                ctx.log(LogLevel::Info, QStringLiteral("line %1").arg(i));
                QThread::msleep(5);
            }
            ctx.log(LogLevel::Warning, QStringLiteral("AVX2 path selected (AVX-512 not present)"));
            return okResult(1);
        };
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy lines(&o, &Orchestrator::logLine);
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("cpu")}), meta()).ok);
        QVERIFY(waitFinished(finished));

        QVector<LogLine> streamed;
        for (const auto &args : lines)
            streamed.append(args.at(0).value<LogLine>());
        // Everything streamed live is also saved with the run (D16), in order.
        QCOMPARE(sink.logLines, streamed);
        QVERIFY(sink.logBatches > 1);
        QVERIFY(sink.logBatches < streamed.size());
        bool sawTestLine = false;
        for (const LogLine &l : std::as_const(streamed)) {
            if (l.message.startsWith(QLatin1StringView("AVX2"))) {
                sawTestLine = true;
                QCOMPARE(l.level, LogLevel::Warning);
                QCOMPARE(l.testId, QStringLiteral("cpu"));
            }
        }
        QVERIFY(sawTestLine);
    }

    void progressiveCommitsToSqlite()
    {
        QTemporaryDir dir;
        const QString db = dir.filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(db).ok());
        SqliteRunSink sqlite(db);
        Fixture f;
        RecordingSink sink(f.events, &sqlite);

        int seenResults = -1;
        QString seenStatus;
        f[QStringLiteral("ram")]->runner = [&](RunContext &, const OptionValues &) {
            // While RAM runs, CPU's result is already committed and the run
            // row says 'running'.
            Database::ScopedConnection c(db, QStringLiteral("peek"));
            QSqlQuery q(c.db());
            q.exec(QStringLiteral("SELECT COUNT(*) FROM test_results"));
            q.next();
            seenResults = q.value(0).toInt();
            q.exec(QStringLiteral("SELECT status FROM runs"));
            q.next();
            seenStatus = q.value(0).toString();
            return okResult(200);
        };

        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        const auto r = o.start(plan({QStringLiteral("cpu"), QStringLiteral("ram")}), meta());
        QVERIFY(r.ok);
        QVERIFY(waitFinished(finished));
        QCOMPARE(seenResults, 1);
        QCOMPARE(seenStatus, QStringLiteral("running"));

        Database::ScopedConnection c(db, QStringLiteral("check"));
        RunRepository repo(c.db());
        const auto stored = repo.load(r.runId);
        QVERIFY(stored);
        QVERIFY(*stored == finishedRun(finished));
        QCOMPARE(stored->status, RunStatus::Completed);
        QVERIFY(!repo.loadLog(*repo.runId(r.runId)).isEmpty());
    }

    void stopSavesStoppedRun()
    {
        QTemporaryDir dir;
        const QString db = dir.filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(db).ok());
        SqliteRunSink sqlite(db);
        Fixture f;
        RecordingSink sink(f.events, &sqlite);
        f[QStringLiteral("ram")]->runner = untilCancelled;

        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QSignalSpy states(&o, &Orchestrator::stateChanged);
        connect(&o, &Orchestrator::testStarted, &o, [&](int index) {
            if (index == 1)
                QTimer::singleShot(50, &o, [&] { o.stop(); });
        });
        const auto r = o.start(plan({QStringLiteral("cpu"), QStringLiteral("ram"), QStringLiteral("gpu")}), meta());
        QVERIFY(r.ok);
        QVERIFY(waitFinished(finished));

        const RunRecord run = finishedRun(finished);
        QCOMPARE(run.status, RunStatus::Stopped);
        QCOMPARE(statuses(run), (QStringList{QStringLiteral("cpu=ok"), QStringLiteral("ram=cancelled"),
                                             QStringLiteral("gpu=not_run")}));
        QCOMPARE(run.tests[1].statusReason, QStringLiteral("Stopped"));
        QVERIFY(!run.tests[1].score);
        QVERIFY(!run.tests[2].score);
        QCOMPARE(run.overall, std::optional<double>(100));  // the finished tests only (N2: "partial" in the UI)
        QVERIFY(states.contains(QVariantList{QVariant::fromValue(Orchestrator::State::Stopping)}));
        QCOMPARE(o.state(), Orchestrator::State::Idle);

        Database::ScopedConnection c(db, QStringLiteral("check"));
        const auto stored = RunRepository(c.db()).load(r.runId);
        QVERIFY(stored);
        QVERIFY(*stored == run);
    }

    void timeoutCancelsAndContinues()
    {
        Fixture f;
        f[QStringLiteral("cpu")]->limit = 1s;
        f[QStringLiteral("cpu")]->runner = untilCancelled;
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("cpu"), QStringLiteral("ram")}), meta()).ok);
        QVERIFY(waitFinished(finished));

        const RunRecord run = finishedRun(finished);
        QCOMPARE(run.status, RunStatus::Completed);
        QCOMPARE(statuses(run), (QStringList{QStringLiteral("cpu=timed_out"), QStringLiteral("ram=ok")}));
        QVERIFY(run.tests[0].statusReason.contains(QLatin1StringView("time limit")));
        QVERIFY(run.tests[0].durationMs >= 1000);
        QVERIFY(!run.tests[0].score);
        QCOMPARE(run.overall, std::optional<double>(200));
    }

    void testIgnoringTimeoutIsAbandoned()
    {
        Fixture f;
        std::atomic<bool> release{false};
        std::atomic<bool> returned{false};
        f[QStringLiteral("cpu")]->limit = 1s;
        f[QStringLiteral("cpu")]->runner = [&](RunContext &ctx, const OptionValues &) {
            while (!release)  // ignores cancel
                QThread::msleep(5);
            ctx.log(LogLevel::Info, QStringLiteral("late line"));
            ctx.progress(1.0, QStringLiteral("late"));
            returned = true;
            return okResult(1);
        };
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QSignalSpy logLines(&o, &Orchestrator::logLine);
        QVERIFY(o.start(plan({QStringLiteral("cpu"), QStringLiteral("ram")}), meta()).ok);
        QVERIFY(waitFinished(finished));

        const RunRecord run = finishedRun(finished);
        QCOMPARE(run.status, RunStatus::Interrupted);
        QCOMPARE(statuses(run), (QStringList{QStringLiteral("cpu=interrupted"), QStringLiteral("ram=not_run")}));
        QCOMPARE(run.tests[0].statusReason, QStringLiteral("Did not respond, restart SynBench"));
        QCOMPARE(o.state(), Orchestrator::State::Abandoned);
        QCOMPARE(sink.finished.status, RunStatus::Interrupted);
        QCOMPARE(sink.commits.size(), 2);
        QVERIFY(!f.inhibitor.held);

        // No new run until a restart.
        const auto again = o.start(plan({QStringLiteral("ram")}), meta());
        QVERIFY(!again.ok);
        QVERIFY(again.errors.first().contains(QLatin1StringView("Restart")));

        // The stuck test finally returns: nothing more reaches the sink or the UI.
        const int eventsBefore = f.events.size();
        const int linesBefore = logLines.count();
        release = true;
        QTRY_VERIFY(returned.load());
        QTest::qWait(100);
        QStringList late = f.events.names().mid(eventsBefore);
        late.removeAll(QStringLiteral("threadFinished"));
        QCOMPARE(late, QStringList());
        QCOMPARE(logLines.count(), linesBefore);
    }

    void testIgnoringStopIsAbandoned()
    {
        Fixture f;
        std::atomic<bool> release{false};
        std::atomic<bool> returned{false};
        f[QStringLiteral("cpu")]->runner = [&](RunContext &, const OptionValues &) {
            while (!release)
                QThread::msleep(5);
            returned = true;
            return okResult(1);
        };
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        connect(&o, &Orchestrator::testStarted, &o, [&] { QTimer::singleShot(20, &o, [&] { o.stop(); }); });
        QVERIFY(o.start(plan({QStringLiteral("cpu")}), meta()).ok);
        QVERIFY(waitFinished(finished));
        QCOMPARE(finishedRun(finished).status, RunStatus::Interrupted);
        QCOMPARE(o.state(), Orchestrator::State::Abandoned);
        release = true;
        QTRY_VERIFY(returned.load());
    }

    void errorsDontStopTheRun()
    {
        Fixture f;
        Registry registry;
        auto add = [&](const QString &id, std::function<TestResult(RunContext &, const OptionValues &)> runner) {
            auto t = std::make_unique<FakeTest>(FakeTest::makeInfo(id));
            t->runner = std::move(runner);
            t->scorer = [](const TestResult &) {
                ScoreBreakdown b;
                b.total = 50;
                return b;
            };
            registry.add(std::move(t));
        };
        add(QStringLiteral("err"), [](RunContext &, const OptionValues &) { return TestResult::error(QStringLiteral("GPU hang")); });
        add(QStringLiteral("throws"), [](RunContext &, const OptionValues &) -> TestResult {
            throw std::runtime_error("boom");
        });
        add(QStringLiteral("throwsint"), [](RunContext &, const OptionValues &) -> TestResult { throw 42; });
        add(QStringLiteral("na"), [](RunContext &, const OptionValues &) {
            return TestResult::notAvailable(QStringLiteral("No supported NPU found"));
        });
        add(QStringLiteral("ok"), [](RunContext &, const OptionValues &) { return okResult(1); });

        RecordingSink sink(f.events);
        Orchestrator o(registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan(registry.ids()), meta()).ok);
        QVERIFY(waitFinished(finished));

        const RunRecord run = finishedRun(finished);
        QCOMPARE(run.status, RunStatus::Completed);
        QCOMPARE(statuses(run), (QStringList{QStringLiteral("err=error"), QStringLiteral("throws=error"),
                                             QStringLiteral("throwsint=error"), QStringLiteral("na=not_available"),
                                             QStringLiteral("ok=ok")}));
        QCOMPARE(run.tests[1].statusReason, QStringLiteral("boom"));
        QCOMPARE(run.tests[2].statusReason, QStringLiteral("Unknown error"));
        // N/A and Error never get a score, even if the formula would give one.
        for (int i = 0; i < 4; ++i)
            QVERIFY(!run.tests[i].score);
        QCOMPARE(run.tests[4].score, std::optional<double>(50));
        QCOMPARE(run.overall, std::optional<double>(50));
    }

    void subScoresAndScorerFailure()
    {
        Fixture f;
        f[QStringLiteral("cpu")]->runner = [](RunContext &, const OptionValues &) {
            TestResult r = okResult(10);
            r.subResults = {{QStringLiteral("a"), Status::Ok, {}, 999.0}, {QStringLiteral("b"), Status::Ok, {}, {}}};
            return r;
        };
        f[QStringLiteral("cpu")]->scorer = [](const TestResult &) {
            ScoreBreakdown b;
            b.total = 30;
            b.subScores.insert(QStringLiteral("a"), 10);
            b.subScores.insert(QStringLiteral("b"), 20);
            return b;
        };
        f[QStringLiteral("ram")]->scorer = [](const TestResult &) -> ScoreBreakdown {
            throw std::runtime_error("bad formula");
        };
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("cpu"), QStringLiteral("ram")}), meta()).ok);
        QVERIFY(waitFinished(finished));
        const RunRecord run = finishedRun(finished);
        QCOMPARE(run.tests[0].score, std::optional<double>(30));
        // Sub-scores come from score(), not from what run() put there.
        QCOMPARE(run.tests[0].subResults[0].score, std::optional<double>(10));
        QCOMPARE(run.tests[0].subResults[1].score, std::optional<double>(20));
        QCOMPARE(run.tests[1].status, Status::Ok);
        QVERIFY(!run.tests[1].score);
        QCOMPARE(run.overall, std::optional<double>(30));
    }

    void exclusivityAndValidation()
    {
        Fixture f;
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());

        auto r = o.start(plan({QStringLiteral("cpu"), QStringLiteral("battery")}), meta());
        QVERIFY(!r.ok);
        QVERIFY(r.errors.join(u' ').contains(QLatin1StringView("always runs on its own")));

        QVERIFY(!o.start({}, meta()).ok);
        QVERIFY(!o.start(plan({QStringLiteral("nope")}), meta()).ok);
        QVERIFY(!o.start(plan({QStringLiteral("cpu"), QStringLiteral("cpu")}), meta()).ok);

        f[QStringLiteral("cpu")]->validator = [](const OptionValues &) {
            Validation v;
            v.add(QStringLiteral("x"), QStringLiteral("bad option"));
            return v;
        };
        r = o.start(plan({QStringLiteral("cpu")}), meta());
        QVERIFY(!r.ok);
        QVERIFY(r.errors.first().contains(QLatin1StringView("bad option")));
        f[QStringLiteral("cpu")]->validator = nullptr;

        f[QStringLiteral("ram")]->preconds = {{QStringLiteral("ac"), QStringLiteral("Running on battery"),
                                               Precondition::State::NotMet, QStringLiteral("On AC: unplug")}};
        r = o.start(plan({QStringLiteral("ram")}), meta());
        QVERIFY(!r.ok);
        QVERIFY(r.errors.first().contains(QLatin1StringView("On AC: unplug")));

        // Nothing was started by any of that.
        QCOMPARE(f.events.size(), 0);
        QCOMPARE(o.state(), Orchestrator::State::Idle);

        f[QStringLiteral("ram")]->preconds = {{QStringLiteral("lock"), QStringLiteral("Brightness locked"),
                                               Precondition::State::WillApplyAtStart, {}}};
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("ram")}), meta()).ok);
        QVERIFY(waitFinished(finished));
    }

    void batteryAloneIsExclusiveRun()
    {
        Fixture f;
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("battery")}), meta()).ok);
        QVERIFY(waitFinished(finished));
        const RunRecord run = finishedRun(finished);
        QCOMPARE(run.kind, RunKind::Exclusive);
        QCOMPARE(sink.begun.kind, RunKind::Exclusive);
        QCOMPARE(run.tests[0].score, std::optional<double>(42));
        QVERIFY(!run.overall);  // this fake battery doesn't count toward overall
        // Recoverable test: durable writes while it runs (§3).
        const QStringList ev = f.events.names();
        QVERIFY(ev.indexOf(QStringLiteral("durable:on")) < ev.indexOf(QStringLiteral("commit:0:battery")));
        QVERIFY(ev.contains(QStringLiteral("durable:off")));
    }

    void rejectsSecondStartWhileRunning()
    {
        Fixture f;
        f[QStringLiteral("cpu")]->runner = untilCancelled;
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("cpu")}), meta()).ok);
        const auto second = o.start(plan({QStringLiteral("ram")}), meta());
        QVERIFY(!second.ok);
        QVERIFY(second.errors.first().contains(QLatin1StringView("already in progress")));
        o.stop();
        QVERIFY(waitFinished(finished));
        QCOMPARE(finishedRun(finished).status, RunStatus::Stopped);
        // And a new run works after that.
        finished.clear();
        f[QStringLiteral("cpu")]->runner = [](RunContext &, const OptionValues &) { return okResult(1); };
        QVERIFY(o.start(plan({QStringLiteral("cpu")}), meta()).ok);
        QVERIFY(waitFinished(finished));
    }

    void sleepBlockedForWholeRun()
    {
        Fixture f;
        RecordingSink sink(f.events);
        std::thread::id runThread;
        bool heldDuringTest = false;
        f[QStringLiteral("cpu")]->runner = [&](RunContext &, const OptionValues &) {
            runThread = std::this_thread::get_id();
            heldDuringTest = f.inhibitor.held;
            return okResult(1);
        };
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("cpu"), QStringLiteral("ram")}), meta()).ok);
        QVERIFY(waitFinished(finished));

        const QStringList ev = f.events.names();
        QCOMPARE(ev.count(QStringLiteral("acquire")), 1);
        QCOMPARE(ev.count(QStringLiteral("release")), 1);
        // From Start until the run row is finalised (D8).
        QVERIFY(ev.indexOf(QStringLiteral("acquire")) < ev.indexOf(QStringLiteral("beginRun")));
        QVERIFY(ev.indexOf(QStringLiteral("finishRun")) < ev.indexOf(QStringLiteral("release")));
        QVERIFY(heldDuringTest);
        QVERIFY(!f.inhibitor.held);
        // On the worker thread (Windows' execution state is per-thread).
        QCOMPARE(f.events.threadOf(QStringLiteral("acquire")), runThread);
        QCOMPARE(f.events.threadOf(QStringLiteral("release")), runThread);
        QVERIFY(runThread != std::this_thread::get_id());
    }

    void sleepBlockFailureIsOnlyAWarning()
    {
        Fixture f;
        f.inhibitor.fail = true;
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("cpu")}), meta()).ok);
        QVERIFY(waitFinished(finished));
        QCOMPARE(finishedRun(finished).status, RunStatus::Completed);
        bool warned = false;
        for (const LogLine &l : std::as_const(sink.logLines))
            warned = warned || (l.level == LogLevel::Warning && l.message.contains(QLatin1StringView("no logind")));
        QVERIFY(warned);
    }

    void sleepReleasedOnStop()
    {
        Fixture f;
        f[QStringLiteral("cpu")]->runner = untilCancelled;
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        connect(&o, &Orchestrator::testStarted, &o, [&] { QTimer::singleShot(20, &o, [&] { o.stop(); }); });
        QVERIFY(o.start(plan({QStringLiteral("cpu")}), meta()).ok);
        QVERIFY(waitFinished(finished));
        QVERIFY(!f.inhibitor.held);
        const QStringList ev = f.events.names();
        QVERIFY(ev.indexOf(QStringLiteral("finishRun")) < ev.indexOf(QStringLiteral("release")));
    }

    void beginFailureRunsNothing()
    {
        Fixture f;
        RecordingSink sink(f.events);
        sink.failBegin = true;
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QSignalSpy storageErrors(&o, &Orchestrator::storageError);
        bool ran = false;
        f[QStringLiteral("cpu")]->runner = [&](RunContext &, const OptionValues &) {
            ran = true;
            return okResult(1);
        };
        QVERIFY(o.start(plan({QStringLiteral("cpu")}), meta()).ok);
        QVERIFY(waitFinished(finished));
        QVERIFY(!ran);
        QCOMPARE(finishedRun(finished).status, RunStatus::Interrupted);
        QCOMPARE(statuses(finishedRun(finished)), QStringList{QStringLiteral("cpu=not_run")});
        QVERIFY(storageErrors.count() >= 1);
        QVERIFY(!f.inhibitor.held);
    }

    void checkpointsAndScratch()
    {
        QTemporaryDir scratchRoot;
        Fixture f;
        QString scratch;
        bool sameDirTwice = false;
        bool fileWritten = false;
        f[QStringLiteral("battery")]->runner = [&](RunContext &ctx, const OptionValues &) {
            ctx.checkpoint(QCborMap{{QStringLiteral("charge"), 99}});
            ctx.checkpoint(QCborMap{{QStringLiteral("charge"), 98}});
            scratch = ctx.scratchDir();
            sameDirTwice = ctx.scratchDir() == scratch;  // created once
            QFile file(QDir(scratch).filePath(QStringLiteral("data.bin")));
            fileWritten = file.open(QIODevice::WriteOnly) && file.write("x") == 1;
            return okResult(1);
        };
        RecordingSink sink(f.events);
        auto config = fastConfig();
        config.scratchRoot = scratchRoot.path();
        Orchestrator o(f.registry, sink, f.inhibitor, config);
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("battery")}), meta()).ok);
        QVERIFY(waitFinished(finished));

        QVERIFY(scratch.startsWith(scratchRoot.path()));
        QVERIFY(sameDirTwice);
        QVERIFY(fileWritten);
        QVERIFY(!QFileInfo::exists(scratch));                    // cleaned up after the test
        QVERIFY(QDir(scratchRoot.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
        const QStringList ev = f.events.names();
        QVERIFY(ev.contains(QStringLiteral("checkpoint:battery:0")));
        QVERIFY(ev.contains(QStringLiteral("checkpoint:battery:1")));
        QVERIFY(ev.indexOf(QStringLiteral("addScratch")) < ev.indexOf(QStringLiteral("removeScratch")));
        QCOMPARE(sink.checkpoints.size(), 2);
        QCOMPARE(QCborValue::fromCbor(sink.checkpoints[1]).toMap().value(QStringLiteral("charge")).toInteger(), 98);
    }

    void checkpointsReachSqlite()
    {
        QTemporaryDir dir;
        const QString db = dir.filePath(QStringLiteral("synbench.db"));
        QVERIFY(Database::migrate(db).ok());
        SqliteRunSink sqlite(db);
        Fixture f;
        f[QStringLiteral("battery")]->runner = [&](RunContext &ctx, const OptionValues &) {
            for (int i = 0; i < 3; ++i)
                ctx.checkpoint(QCborMap{{QStringLiteral("i"), i}});
            return okResult(1);
        };
        Orchestrator o(f.registry, sqlite, f.inhibitor, fastConfig());
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("battery")}), meta()).ok);
        QVERIFY(waitFinished(finished));
        Database::ScopedConnection c(db, QStringLiteral("check"));
        QSqlQuery q(c.db());
        QVERIFY(q.exec(QStringLiteral("SELECT seq FROM checkpoints WHERE test_id = 'battery' ORDER BY seq")));
        QList<int> seqs;
        while (q.next())
            seqs.append(q.value(0).toInt());
        QCOMPARE(seqs, (QList<int>{0, 1, 2}));
    }

    void currentRunUpdatesLive()
    {
        Fixture f;
        f[QStringLiteral("ram")]->runner = untilCancelled;
        RecordingSink sink(f.events);
        Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
        QSignalSpy testFinished(&o, &Orchestrator::testFinished);
        QSignalSpy finished(&o, &Orchestrator::runFinished);
        QVERIFY(o.start(plan({QStringLiteral("cpu"), QStringLiteral("ram")}), meta()).ok);
        QVERIFY(testFinished.wait(5000));
        // "View results so far": CPU is in, RAM is still running.
        QCOMPARE(o.currentRun().status, RunStatus::Running);
        QCOMPARE(o.currentRun().tests.size(), 1);
        QCOMPARE(o.currentRun().tests[0].testId, QStringLiteral("cpu"));
        o.stop();
        QVERIFY(waitFinished(finished));
    }

    void destroyWhileRunningStopsCleanly()
    {
        Fixture f;
        f[QStringLiteral("cpu")]->runner = untilCancelled;
        RecordingSink sink(f.events);
        {
            Orchestrator o(f.registry, sink, f.inhibitor, fastConfig());
            QSignalSpy testStarted(&o, &Orchestrator::testStarted);
            QVERIFY(o.start(plan({QStringLiteral("cpu"), QStringLiteral("ram")}), meta()).ok);
            QVERIFY(testStarted.wait(5000));
        }
        QCOMPARE(sink.finished.status, RunStatus::Stopped);
        QVERIFY(!f.inhibitor.held);
    }
};

QTEST_GUILESS_MAIN(TestOrchestrator)
#include "tst_orchestrator.moc"
