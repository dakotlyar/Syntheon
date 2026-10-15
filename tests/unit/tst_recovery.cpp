// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/fs_util.h"
#include "core/orchestrator.h"
#include "core/power_inhibitor.h"
#include "core/registry.h"
#include "fake_test.h"
#include "storage/database.h"
#include "storage/recovery.h"
#include "storage/run_repository.h"
#include "storage/sqlite_run_sink.h"

#include <QCborValue>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <stdexcept>

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

TestResult placeholder(const QString &id, const QVariantMap &options = {})
{
    TestResult r = TestResult::withStatus(Status::NotRun, QStringLiteral("Queued"));
    r.testId = id;
    r.options = options;
    return r;
}

TestResult okResult(const QString &id, double value)
{
    TestResult r;
    r.testId = id;
    r.status = Status::Ok;
    r.score = value;
    r.metrics = {{QStringLiteral("value"), {}, QStringLiteral("Value"), value, QString(), Direction::HigherIsBetter,
                  true}};
    return r;
}

ScoreBreakdown scoreFromValue(const TestResult &r)
{
    ScoreBreakdown b;
    if (const Metric *m = r.metric(QStringLiteral("value")))
        b.total = std::get<double>(m->value);
    return b;
}

struct Fixture {
    Fixture()
    {
        QVERIFY(Database::migrate(dbPath()).ok());
        conn = std::make_unique<Database::ScopedConnection>(dbPath(), QStringLiteral("test"));

        for (const QString &id : {QStringLiteral("cpu"), QStringLiteral("ram"), QStringLiteral("gpu")}) {
            auto t = std::make_unique<FakeTest>(FakeTest::makeInfo(id));
            t->scorer = scoreFromValue;
            fakes.insert(id, t.get());
            registry.add(std::move(t));
        }
        TestInfo bi = FakeTest::makeInfo(QStringLiteral("battery"), Exclusivity::RunsAlone);
        bi.recovery = RecoveryMode::FromCheckpoints;
        auto b = std::make_unique<FakeTest>(bi);
        b->scorer = scoreFromValue;
        fakes.insert(QStringLiteral("battery"), b.get());
        registry.add(std::move(b));
    }
    QString dbPath() const { return dir.filePath(QStringLiteral("synbench.db")); }
    QSqlDatabase db() { return conn->db(); }
    RunRepository repo() { return RunRepository(db()); }

    // A run left 'running' by a crash, with the given rows.
    RunRecord seed(const QVector<TestResult> &tests, RunKind kind = RunKind::Standard)
    {
        RunRecord r;
        r.uuid = QUuid::createUuid();
        r.kind = kind;
        r.status = RunStatus::Running;
        r.startedAt = QDateTime::currentDateTimeUtc().addSecs(-3600);
        r.appVersion = QStringLiteral("2026.11.0");
        r.appBuild = QStringLiteral("linux-x64 test");
        r.system.machineLabel = QStringLiteral("Knowhere");
        r.tests = tests;
        const auto id = repo().saveRun(r);
        [&] { QVERIFY(id); }();
        return r;
    }

    QTemporaryDir dir;
    std::unique_ptr<Database::ScopedConnection> conn;
    Registry registry;
    QHash<QString, FakeTest *> fakes;
};

QStringList statuses(const RunRecord &r)
{
    QStringList out;
    for (const TestResult &t : r.tests)
        out.append(t.testId + QLatin1Char('=') + toKey(t.status));
    return out;
}

} // namespace

class TestRecovery : public QObject
{
    Q_OBJECT

private slots:
    void nothingToDo()
    {
        Fixture f;
        f.repo().saveRun([] {
            RunRecord r;
            r.uuid = QUuid::createUuid();
            r.status = RunStatus::Completed;
            r.startedAt = QDateTime::currentDateTimeUtc();
            return r;
        }());
        const RecoveryReport report = recoverRuns(f.db(), f.registry);
        QVERIFY(report.notices.isEmpty());
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(report.scratchPathsRemoved, 0);
    }

    void nonRecoverableRunIsInterrupted()
    {
        Fixture f;
        const RunRecord seeded =
            f.seed({okResult(QStringLiteral("cpu"), 100), placeholder(QStringLiteral("ram")),
                    placeholder(QStringLiteral("gpu"))});
        RunRepository repo = f.repo();
        const qint64 id = *repo.runId(seeded.uuid);
        repo.appendLog(id, {{QDateTime::currentDateTimeUtc().addSecs(-60), LogLevel::Info, {}, QStringLiteral("x")}});

        const RecoveryReport report = recoverRuns(f.db(), f.registry);
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join(u'\n')));
        QCOMPARE(report.notices.size(), 1);
        QCOMPARE(report.notices[0].status, RunStatus::Interrupted);
        QCOMPARE(report.notices[0].runId, seeded.uuid);
        QCOMPARE(report.notices[0].startedAt, seeded.startedAt);

        const RunRecord run = *repo.load(seeded.uuid);
        QCOMPARE(run.status, RunStatus::Interrupted);
        // Committed results are kept; the rest are Interrupted.
        QCOMPARE(statuses(run), (QStringList{QStringLiteral("cpu=ok"), QStringLiteral("ram=interrupted"),
                                             QStringLiteral("gpu=interrupted")}));
        QCOMPARE(run.tests[1].statusReason, QStringLiteral("SynBench closed before this test finished"));
        QCOMPARE(run.overall, std::optional<double>(100));
        // finished_at = the last sign of life (the log line), not "now".
        QVERIFY(run.finishedAt < QDateTime::currentDateTimeUtc().addSecs(-30));
        QVERIFY(repo.loadLog(id).last().message.contains(QLatin1StringView("interrupted")));

        // Running it again changes nothing.
        QVERIFY(recoverRuns(f.db(), f.registry).notices.isEmpty());
    }

    void batteryRecoveredFromCheckpoints()
    {
        Fixture f;
        const QVariantMap options{{QStringLiteral("profile"), QStringLiteral("mixed")}};
        const RunRecord seeded = f.seed({placeholder(QStringLiteral("battery"), options)}, RunKind::Exclusive);
        RunRepository repo = f.repo();
        const qint64 id = *repo.runId(seeded.uuid);
        // Written out of order on purpose: recover() must see seq order.
        for (int seq : {1, 0, 2})
            QVERIFY(repo.addCheckpoint(id, QStringLiteral("battery"), seq,
                                       QCborValue(QCborMap{{QStringLiteral("charge"), 100 - seq}}).toCbor()));

        QVariantMap seenOptions;
        QList<qint64> seenCharges;
        QString seenMachine;
        f.fakes[QStringLiteral("battery")]->recoverer = [&](RecoveryContext &ctx) {
            seenOptions = ctx.options();
            for (const QCborMap &c : ctx.checkpoints())
                seenCharges.append(c.value(QStringLiteral("charge")).toInteger());
            seenMachine = ctx.system().machineLabel;
            TestResult r;
            r.status = Status::Ok;
            r.durationMs = (6 * 60 + 42) * 60 * 1000;
            r.metrics = {{QStringLiteral("value"), {}, QStringLiteral("Runtime"), 402.0, QStringLiteral("min"),
                          Direction::HigherIsBetter, true}};
            r.subResults = {{QStringLiteral("x"), Status::Ok, {}, 12345.0}};  // not trusted
            return r;
        };

        const RecoveryReport report = recoverRuns(f.db(), f.registry);
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join(u'\n')));
        QCOMPARE(seenOptions, options);
        QCOMPARE(seenCharges, (QList<qint64>{100, 99, 98}));
        QCOMPARE(seenMachine, QStringLiteral("Knowhere"));

        QCOMPARE(report.notices.size(), 1);
        QCOMPARE(report.notices[0].status, RunStatus::Recovered);
        QCOMPARE(report.notices[0].recoveredTestIds, QStringList{QStringLiteral("battery")});
        QCOMPARE(report.notices[0].recoveredDurationMs, qint64((6 * 60 + 42) * 60 * 1000));

        const RunRecord run = *repo.load(seeded.uuid);
        QCOMPARE(run.status, RunStatus::Recovered);
        QCOMPARE(run.tests[0].status, Status::Ok);
        QCOMPARE(run.tests[0].score, std::optional<double>(402));  // scored by the usual rules
        QVERIFY(!run.tests[0].subResults[0].score);
        QCOMPARE(run.tests[0].options, options);
        QCOMPARE(run.overall, std::optional<double>(402));
        QVERIFY(run.finishedAt.isValid());
    }

    void recoverDefaultMeansInterrupted()
    {
        Fixture f;
        const RunRecord seeded = f.seed({placeholder(QStringLiteral("battery"))}, RunKind::Exclusive);
        // No recoverer: ITest::recover() returns Interrupted.
        const RecoveryReport report = recoverRuns(f.db(), f.registry);
        QCOMPARE(report.notices[0].status, RunStatus::Interrupted);
        QCOMPARE(f.repo().load(seeded.uuid)->status, RunStatus::Interrupted);
    }

    void recoverThatThrowsIsAnError()
    {
        Fixture f;
        const RunRecord seeded = f.seed({placeholder(QStringLiteral("battery"))}, RunKind::Exclusive);
        f.fakes[QStringLiteral("battery")]->recoverer = [](RecoveryContext &) -> TestResult {
            throw std::runtime_error("bad checkpoint");
        };
        const RecoveryReport report = recoverRuns(f.db(), f.registry);
        const RunRecord run = *f.repo().load(seeded.uuid);
        QCOMPARE(run.tests[0].status, Status::Error);
        QVERIFY(run.tests[0].statusReason.contains(QLatin1StringView("bad checkpoint")));
        QVERIFY(!run.tests[0].score);
        QCOMPARE(report.notices[0].status, RunStatus::Recovered);  // it produced a (failed) result
    }

    void recoveredNonFiniteIsError()
    {
        Fixture f;
        const RunRecord seeded = f.seed({placeholder(QStringLiteral("battery"))}, RunKind::Exclusive);
        f.fakes[QStringLiteral("battery")]->recoverer = [](RecoveryContext &) {
            TestResult r;
            r.status = Status::Ok;
            r.metrics = {{QStringLiteral("value"), {}, QStringLiteral("Runtime"), qQNaN(), {}, Direction::Neutral, true}};
            return r;
        };
        recoverRuns(f.db(), f.registry);
        const RunRecord run = *f.repo().load(seeded.uuid);
        QCOMPARE(run.tests[0].status, Status::Error);
        QCOMPARE(run.tests[0].statusReason, QStringLiteral("produced an invalid value for Runtime"));
    }

    void unknownTestIsInterrupted()
    {
        Fixture f;
        const RunRecord seeded = f.seed({placeholder(QStringLiteral("removed_test"))});
        const RecoveryReport report = recoverRuns(f.db(), f.registry);
        QCOMPARE(report.notices[0].status, RunStatus::Interrupted);
        QCOMPARE(f.repo().load(seeded.uuid)->tests[0].status, Status::Interrupted);
    }

    // F5: scratch folders of an abandoned run (already 'interrupted', not
    // 'running') are cleaned at the next start.
    void abandonedRunScratchCleaned()
    {
        Fixture f;
        QTemporaryDir scratchRoot;
        SqliteRunSink sink(f.dbPath());
        NoInhibitor inhibitor;
        std::atomic<bool> release{false};
        std::atomic<bool> returned{false};
        QString scratch;
        FakeTest *cpu = f.fakes[QStringLiteral("cpu")];
        cpu->limit = 1s;
        cpu->runner = [&](RunContext &ctx, const OptionValues &) {
            scratch = ctx.scratchDir();
            QFile file(QDir(scratch).filePath(QStringLiteral("big.bin")));
            if (file.open(QIODevice::WriteOnly))
                file.write("data");
            file.close();
            while (!release)  // ignores the timeout's cancel
                QThread::msleep(5);
            returned = true;
            TestResult r;
            r.status = Status::Ok;
            return r;
        };
        Orchestrator::Config config;
        config.grace = 200ms;
        config.watchdogInterval = 10ms;
        config.scratchRoot = scratchRoot.path();
        QUuid runId;
        {
            Orchestrator o(f.registry, sink, inhibitor, config);
            QSignalSpy finished(&o, &Orchestrator::runFinished);
            RunMeta meta;
            const auto started = o.start({{QStringLiteral("cpu"), {}}}, meta);
            QVERIFY(started.ok);
            runId = started.runId;
            QVERIFY(finished.wait(10000));
            QCOMPARE(o.state(), Orchestrator::State::Abandoned);
        }
        QVERIFY(QFileInfo::exists(scratch));  // left behind by the abandoned run
        QCOMPARE(f.repo().load(runId)->status, RunStatus::Interrupted);

        // "Next start": release the stuck thread first so it isn't using the folder.
        release = true;
        QTRY_VERIFY(returned.load());
        const RecoveryReport report = recoverRuns(f.db(), f.registry, scratchRoot.path());
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join(u'\n')));
        QCOMPARE(report.scratchPathsRemoved, 1);
        QVERIFY(!QFileInfo::exists(scratch));
        QVERIFY(QDir(scratchRoot.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
        QSqlQuery q(f.db());
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM scratch_paths")) && q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }

    void scratchOfRunningRunRecoveredThenCleaned()
    {
        Fixture f;
        QTemporaryDir root;
        const RunRecord seeded = f.seed({placeholder(QStringLiteral("cpu"))});
        const QString path = QDir(root.path()).filePath(
            QStringLiteral("synbench-%1/cpu").arg(seeded.uuid.toString(QUuid::WithoutBraces)));
        QVERIFY(QDir().mkpath(path));
        RunRepository repo = f.repo();
        QVERIFY(repo.addScratchPath(*repo.runId(seeded.uuid), path));
        const RecoveryReport report = recoverRuns(f.db(), f.registry, root.path());
        QCOMPARE(report.scratchPathsRemoved, 1);
        QVERIFY(!QFileInfo::exists(path));
    }

    // AP5-R2-2: a registered scratch path that is (or contains) a link must
    // never take the link's target with it.
    void scratchSymlinksNeverFollowed()
    {
        Fixture f;
        QTemporaryDir root, victimDir;
        const auto touch = [](const QString &p) {
            QFile file(p);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("precious");
        };
        touch(QDir(victimDir.path()).filePath(QStringLiteral("keep.txt")));

        RunRecord done;
        done.uuid = QUuid::createUuid();
        done.status = RunStatus::Interrupted;
        done.startedAt = QDateTime::currentDateTimeUtc();
        RunRepository repo = f.repo();
        const auto id = repo.saveRun(done);
        const QString runFolder = QDir(root.path()).filePath(fs::scratchRunFolderName(done.uuid));
        QVERIFY(QDir().mkpath(runFolder));

        // 1. The scratch path itself is a link to the victim.
        const QString topLink = QDir(runFolder).filePath(QStringLiteral("cpu"));
        QVERIFY(QFile::link(victimDir.path(), topLink));
        QVERIFY(repo.addScratchPath(*id, topLink));
        // 2. A real scratch folder containing a link to the victim.
        const QString real = QDir(runFolder).filePath(QStringLiteral("ram"));
        QVERIFY(QDir().mkpath(real));
        touch(QDir(real).filePath(QStringLiteral("junk.bin")));
        QVERIFY(QFile::link(victimDir.path(), QDir(real).filePath(QStringLiteral("inner-link"))));
        QVERIFY(repo.addScratchPath(*id, real));
        // 3. The run folder replaced by a link (path shape still matches).
        RunRecord other;
        other.uuid = QUuid::createUuid();
        other.status = RunStatus::Interrupted;
        other.startedAt = QDateTime::currentDateTimeUtc();
        const auto otherId = repo.saveRun(other);
        const QString linkedRun = QDir(root.path()).filePath(fs::scratchRunFolderName(other.uuid));
        QVERIFY(QFile::link(victimDir.path(), linkedRun));
        QVERIFY(repo.addScratchPath(*otherId, QDir(linkedRun).filePath(QStringLiteral("keep.txt"))));

        const RecoveryReport report = recoverRuns(f.db(), f.registry, root.path());
        QCOMPARE(report.scratchPathsRemoved, 1);  // only the real folder
        QCOMPARE(report.errors.size(), 2);
        QVERIFY(QFile::exists(QDir(victimDir.path()).filePath(QStringLiteral("keep.txt"))));
        QVERIFY(!QFileInfo::exists(real));
        QSqlQuery q(f.db());
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM scratch_paths")) && q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }

    void scratchOutsideRunFolderIsNeverDeleted()
    {
        Fixture f;
        QTemporaryDir precious;
        QFile keep(QDir(precious.path()).filePath(QStringLiteral("keep.txt")));
        QVERIFY(keep.open(QIODevice::WriteOnly));
        keep.close();
        RunRecord done;
        done.uuid = QUuid::createUuid();
        done.status = RunStatus::Completed;
        done.startedAt = QDateTime::currentDateTimeUtc();
        RunRepository repo = f.repo();
        const auto id = repo.saveRun(done);
        QVERIFY(repo.addScratchPath(*id, precious.path()));
        QVERIFY(repo.addScratchPath(*id, QStringLiteral("relative/synbench-x")));

        const RecoveryReport report = recoverRuns(f.db(), f.registry);
        QCOMPARE(report.scratchPathsRemoved, 0);
        QCOMPARE(report.errors.size(), 2);
        QVERIFY(QFile::exists(keep.fileName()));
        QSqlQuery q(f.db());
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM scratch_paths")) && q.next());
        QCOMPARE(q.value(0).toInt(), 0);  // unregistered all the same
    }
};

QTEST_GUILESS_MAIN(TestRecovery)
#include "tst_recovery.moc"
