// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU test (Phase 4a task 15) through a fake RunContext, with a fake
// worker (fake_gpuworker.cpp): scene-by-scene order, the interface hold,
// monotonic progress, Stop within 250 ms, N/A and Error mapping (D6), the
// estimate and the time limit.
#include "core/units.h"
#include "fake_test.h"
#include "privileged/privileged_ops.h"
#include "tests/common/stub_clock.h"
#include "tests/gpu/client/gpu_client.h"
#include "tests/gpu/gpu_test.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <mutex>
#include <thread>

using namespace sb;
using namespace sb::tests;
namespace pr = sb::gpu::protocol;

namespace {

const QString kFake = QStringLiteral(SB_FAKE_GPUWORKER);

class Ctx : public RunContext
{
public:
    void progress(double fraction, const QString &) override
    {
        std::lock_guard lock(m_mutex);
        if (!m_fractions.isEmpty() && fraction + 1e-9 < m_fractions.last())
            m_backwards = true;
        m_fractions.append(fraction);
    }
    void setPart(const QString &part) override
    {
        std::lock_guard lock(m_mutex);
        m_parts.append(part);
    }
    void holdInterface(bool hold) override
    {
        std::lock_guard lock(m_mutex);
        m_holds.append(hold);
    }
    void log(LogLevel, const QString &message) override
    {
        std::lock_guard lock(m_mutex);
        m_logs.append(message);
    }
    bool cancelled() const override { return m_cancel.load(); }
    void subTestBegin(const QString &) override {}
    void subTestEnd(const QString &, Status) override {}
    void checkpoint(const QCborMap &) override {}
    QString scratchDir() override { return {}; }
    const SystemSnapshot &system() const override { return m_system; }
    PrivilegedOps &privileged() override { return m_priv; }

    bool backwards() const { return m_backwards; }
    QVector<bool> holds() const { return m_holds; }
    QStringList parts() const { return m_parts; }
    QVector<double> fractions() const { return m_fractions; }
    QStringList logs() const { return m_logs; }
    void cancel() { m_cancel = true; }

private:
    mutable std::mutex m_mutex;
    QVector<double> m_fractions;
    QVector<bool> m_holds;
    QStringList m_parts, m_logs;
    bool m_backwards = false;
    std::atomic<bool> m_cancel{false};
    SystemSnapshot m_system;
    UnavailablePrivilegedOps m_priv;
};

double number(const TestResult &r, const QString &key)
{
    const Metric *m = r.metric(key);
    return m && m->isNumeric() ? std::get<double>(m->value) : -1.0;
}

QString textOf(const TestResult &r, const QString &key)
{
    const Metric *m = r.metric(key);
    return m && !m->isNumeric() ? std::get<QString>(m->value) : QString();
}

} // namespace

class TestGpuTest : public QObject
{
    Q_OBJECT

    // The probe, as the registry does it before a run.
    static void probed(GpuTest &t)
    {
        sb::testing::FakeProbeContext ctx;
        const Availability a = t.probe(ctx);
        QVERIFY2(a.state != Availability::State::Unavailable, qPrintable(a.reason));
    }

private slots:
    void init()
    {
        tests::setStubTimeScale(0.02);  // 5 s -> 100 ms, 20 s -> 400 ms
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("ok")});
        qunsetenv("SYNBENCH_GPU_FAKE_APIS");
        qunsetenv("SYNBENCH_GPU_WORKER_ENV");
    }
    void cleanup()
    {
        gpu::GpuClient::setWorkerForTests({});
        qunsetenv("SYNBENCH_GPU_FAKE_APIS");
        qunsetenv("SYNBENCH_GPU_WORKER_ENV");
        tests::setStubTimeScale(0.02);
    }

    void estimates()
    {
        QCOMPARE(GpuTest::estimateSeconds(4, 2), 610);
        QCOMPARE(GpuTest::estimateSeconds(2, 1), 310);
        QCOMPARE(GpuTest::estimateSeconds(1, 1), 190);
        QCOMPARE(GpuTest::estimateSeconds(3, 2), 480);
        const auto plan = GpuTest::plan3d();
        QCOMPARE(plan.size(), 4);
        QCOMPARE(plan[0].workload, QStringLiteral("geometry"));
        QCOMPARE(plan[0].warmupMs, 5000);
        QCOMPARE(plan[3].workload, QStringLiteral("particles"));
        QCOMPARE(plan[3].warmupMs, 3000);
    }

    void infoAndOptions()
    {
        GpuTest t;
        QCOMPARE(t.info().id, QStringLiteral("gpu"));
        QCOMPARE(t.info().name, QStringLiteral("GPU"));
        QCOMPARE(t.info().summary, QStringLiteral("3D, 2D and fill-rate on 4 graphics APIs"));
        QCOMPARE(t.info().colorToken, QStringLiteral("t-gpu"));
        QCOMPARE(t.info().subTests.size(), 3);
        for (const SubTestInfo &s : t.info().subTests)
            QVERIFY(!s.userSelectable);
        probed(t);
        sb::testing::FakeProbeContext ctx;
        const OptionSchema o = t.options(ctx, Availability{});
        const OptionDef *gpu = o.find(QStringLiteral("gpu"));
        QVERIFY(gpu);
        QCOMPARE(gpu->kind, OptionKind::DynamicSelect);
        QCOMPARE(gpu->items.size(), 1);
        QCOMPARE(gpu->items[0].detail, QStringLiteral("discrete · 16 GB · Vulkan, OpenGL"));
        QCOMPARE(gpu->defaultValue.toString(), QStringLiteral("10de:2c05@0000:01:00.0"));
        QVERIFY(o.find(QStringLiteral("apis")));
        QVERIFY(o.find(QStringLiteral("window"))->infoValue.contains(QLatin1StringView("vsync off")));
        QVERIFY(o.find(QStringLiteral("runs"))->infoValue.startsWith(QLatin1StringView("Every API and part runs (about ")));
    }

    // Task 15a (M6): the Window row reads the snapshot's primary screen.
    void windowRow()
    {
        GpuTest t;
        probed(t);
        const auto row = [&](int w, int h, double scale, bool withDisplay) {
            sb::testing::FakeProbeContext ctx;
            if (withDisplay) {
                ctx.snapshot.data.insert(QStringLiteral("display"),
                                         QJsonObject{{QStringLiteral("name"), QStringLiteral("DP-3")}, {QStringLiteral("avail_width"), w},
                                                     {QStringLiteral("avail_height"), h}, {QStringLiteral("scale"), scale}});
            }
            const OptionSchema o = t.options(ctx, Availability{});
            return *o.find(QStringLiteral("window"));
        };
        const OptionDef fits = row(2560, 1400, 1.0, true);
        QVERIFY2(fits.infoValue.contains(QStringLiteral("here: 1920×1080 px, fits")), qPrintable(fits.infoValue));
        QVERIFY(fits.infoValue.startsWith(QStringLiteral("1920×1080 logical · vsync off")));
        QCOMPARE(fits.infoTone, Tone::Neutral);
        const OptionDef scaledRow = row(1707, 1019, 1.5, true);
        QVERIFY2(scaledRow.infoValue.contains(QStringLiteral("here: 2880×1620 px, window scaled to fit")), qPrintable(scaledRow.infoValue));
        QCOMPARE(scaledRow.infoTone, Tone::Warn);
        QVERIFY(row(500, 300, 1.0, true).infoValue.contains(QLatin1StringView("too small")));
        const OptionDef none = row(0, 0, 1.0, false);
        QCOMPARE(none.infoValue, QStringLiteral("1920×1080 logical at the screen's scale · vsync off"));
        QCOMPARE(none.infoTone, Tone::Neutral);
    }

    void timeLimitAtRealTime()
    {
#if !defined(Q_OS_WIN)
        GpuTest t;
        probed(t);  // two APIs OK on the fake GPU (Vulkan and OpenGL) + 1 native path
        tests::setStubTimeScale(1.0);
        QCOMPARE(t.estimate({}).count(), 310);
        QCOMPARE(t.timeLimit({}).count(), 680);
#endif
    }

    // One Vulkan run: Partial (native 2D and fill-rate aren't built), the 3D numbers, the
    // hold pair, the parts, monotonic progress.
    void vulkanRun()
    {
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QVERIFY(r.statusReason.startsWith(QLatin1StringView("No GPU score")));
        QCOMPARE(r.subResults.size(), 3);
        QCOMPARE(r.subResults[0].id, QStringLiteral("scenes3d"));
        QCOMPARE(r.subResults[0].status, Status::Ok);
        QCOMPARE(r.subResults[1].status, Status::NotAvailable);
        QCOMPARE(r.subResults[2].status, Status::NotAvailable);
        QCOMPARE(number(r, QStringLiteral("scenes3d.geometry.fps")), number(r, QStringLiteral("scenes3d.vulkan.geometry.fps")));
        QCOMPARE(number(r, QStringLiteral("scenes3d.geometry.fps")), 100.0);
        QCOMPARE(textOf(r, QStringLiteral("scenes3d.geometry.api")), QStringLiteral("Vulkan"));
        QVERIFY(r.metric(QStringLiteral("scenes3d.vulkan.fps")));  // the stub key kept: all four scenes are there
        QCOMPARE(textOf(r, QStringLiteral("api.vulkan.status")), QStringLiteral("OK"));
        QCOMPARE(textOf(r, QStringLiteral("api.opengl.status")), QStringLiteral("N/A: not built yet (Phase 4a-2)"));
#if !defined(Q_OS_WIN)
        QCOMPARE(textOf(r, QStringLiteral("api.dx12.status")), QStringLiteral("N/A: DirectX is Windows only"));
#endif
        QCOMPARE(textOf(r, QStringLiteral("gpu.name")), QStringLiteral("Fake GPU"));
        bool series = false;
        for (const Series &s : r.series)
            series = series || (s.key == QLatin1StringView("ft.vulkan.geometry") && !s.points.isEmpty());
        QVERIFY(series);
        // score.3d is the 3D sub-score (all four scenes at the provisional anchor).
        const ScoreBreakdown b = t.score(r);
        QVERIFY(b.subScores.contains(QStringLiteral("scenes3d")));
        QCOMPARE(number(r, QStringLiteral("score.3d")), b.subScores.value(QStringLiteral("scenes3d")));
        QCOMPARE(b.subScores.value(QStringLiteral("scenes3d")), 1000.0);
        QVERIFY(!b.total);  // Partial: no GPU score
        QCOMPARE(ctx.holds(), (QVector<bool>{true, false}));
        QCOMPARE(ctx.parts(), (QStringList{QStringLiteral("Geometry · Vulkan"), QStringLiteral("Draw calls · Vulkan"),
                                           QStringLiteral("Shading · Vulkan"), QStringLiteral("Particles + post · Vulkan")}));
        QVERIFY(!ctx.backwards());
        QVERIFY(ctx.fractions().last() < 1.0);
    }

    void stopMidRun()
    {
        GpuTest t;
        probed(t);
        Ctx ctx;
        std::atomic<qint64> cancelAt{0};
        QElapsedTimer clock;
        clock.start();
        std::thread stopper([&] {
            QThread::msleep(700);
            cancelAt = clock.elapsed();
            ctx.cancel();
        });
        const TestResult r = t.run(ctx, {});
        const qint64 returned = clock.elapsed();
        stopper.join();
        QCOMPARE(r.status, Status::Cancelled);
        QVERIFY2(returned - cancelAt <= 250, qPrintable(QString::number(returned - cancelAt)));
        QCOMPARE(ctx.holds().last(), false);
    }

    void notAvailableWhenTheApiIs()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("na")});
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::NotAvailable);
        QVERIFY2(textOf(r, QStringLiteral("api.vulkan.status")).startsWith(QLatin1StringView("N/A: ")), qPrintable(textOf(r, QStringLiteral("api.vulkan.status"))));
        QCOMPARE(textOf(r, QStringLiteral("api.vulkan.status")), QStringLiteral("N/A: No Vulkan driver is installed"));
    }

    // D12: each scene on every API before the next scene.
    void sceneByScene()
    {
        QTemporaryDir dir;
        const QString log = dir.filePath(QStringLiteral("commands.txt"));
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        qputenv("SYNBENCH_GPU_WORKER_ENV", ("SB_FAKE_LOG=" + log).toUtf8());
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.subResults[0].status, Status::Ok);
        QFile f(log);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines, (QStringList{QStringLiteral("vulkan geometry"), QStringLiteral("opengl geometry"), QStringLiteral("vulkan drawcalls"),
                                     QStringLiteral("opengl drawcalls"), QStringLiteral("vulkan shading"), QStringLiteral("opengl shading"),
                                     QStringLiteral("vulkan particles"), QStringLiteral("opengl particles")}));
        QCOMPARE(ctx.holds(), (QVector<bool>{true, false}));
        QVERIFY(!ctx.backwards());
    }

    // D6: the second API crashing at shading makes the result Partial with no score; every
    // Vulkan number is still there and the sub-test is an Error.
    void secondApiCrashes()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("crash-at-shading")});
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QVERIFY2(r.statusReason.startsWith(QLatin1StringView("No GPU score: OpenGL failed in Shading: the GPU worker exited with code")),
                 qPrintable(r.statusReason));
        QCOMPARE(r.subResults[0].status, Status::Error);
        QVERIFY(textOf(r, QStringLiteral("api.opengl.status")).startsWith(QLatin1StringView("Error: ")));
        for (const char *scene : {"geometry", "drawcalls", "shading", "particles"})
            QVERIFY(r.metric(QStringLiteral("scenes3d.vulkan.%1.fps").arg(QLatin1String(scene))));
        QVERIFY(r.metric(QStringLiteral("scenes3d.opengl.drawcalls.fps")));      // what it measured before the crash
        QVERIFY(!r.metric(QStringLiteral("scenes3d.opengl.shading.fps")));
        QVERIFY(!t.score(r).subScores.contains(QStringLiteral("scenes3d")));    // a group that isn't Ok has no sub-score
        QVERIFY(!r.metric(QStringLiteral("score.3d")));
    }
    // M2: the faster API that drew the scene differently is the outlier (an Error) and never
    // the headline "best API" number.
    void mismatchingApiIsNeverBest()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("mismatch")});
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QVERIFY(textOf(r, QStringLiteral("api.opengl.status")).startsWith(QLatin1StringView("Error: drew Geometry differently from Vulkan")));
        QCOMPARE(number(r, QStringLiteral("scenes3d.opengl.geometry.fps")), 200.0);  // still stored
        QVERIFY(r.metric(QStringLiteral("scenes3d.opengl.geometry.match")));
        QCOMPARE(number(r, QStringLiteral("scenes3d.geometry.fps")), 100.0);          // Vulkan's, not the faster one
        QCOMPARE(textOf(r, QStringLiteral("scenes3d.geometry.api")), QStringLiteral("Vulkan"));
        QCOMPARE(r.subResults[0].status, Status::Error);
    }

    // L6: Stop with two APIs running still returns within 250 ms.
    void stopWithTwoApis()
    {
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        std::atomic<qint64> cancelAt{0};
        QElapsedTimer clock;
        clock.start();
        std::thread stopper([&] {
            QThread::msleep(700);
            cancelAt = clock.elapsed();
            ctx.cancel();
        });
        const TestResult r = t.run(ctx, {});
        const qint64 returned = clock.elapsed();
        stopper.join();
        QCOMPARE(r.status, Status::Cancelled);
        QVERIFY2(returned - cancelAt <= 250, qPrintable(QString::number(returned - cancelAt)));
    }

    // L8: when every API says N/A the test's reason is theirs.
    void notAvailableReasonIsTheApis()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("na")});
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::NotAvailable);
        QVERIFY2(r.statusReason.contains(QLatin1StringView("No Vulkan driver is installed")), qPrintable(r.statusReason));
    }
};

QTEST_GUILESS_MAIN(TestGpuTest)
#include "tst_gpu_test.moc"
