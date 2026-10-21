// SPDX-License-Identifier: GPL-3.0-or-later
// GpuClient and GpuSession (Phase 4a task 14): the real worker on the null
// backend, and a fake worker with planted defects (junk, hang, crash, na,
// crash after ready). Stop within 250 ms, no process left behind, the
// environment built from scratch.
#include "tests/gpu/client/gpu_client.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QTest>

using namespace sb::gpu;
using Status = ClientOutcome::Status;
namespace pr = sb::gpu::protocol;

namespace {

const QString kWorker = QStringLiteral(SB_GPU_WORKER);
const QString kFake = QStringLiteral(SB_FAKE_GPUWORKER);

ClientRunSpec spec(const QString &plan)
{
    ClientRunSpec s;
    s.api = QStringLiteral("vulkan");
    s.gpuId = QStringLiteral("0000:0000#0");
    s.plan = *pr::parsePlan(plan, nullptr);
    return s;
}

// A process that is gone (Linux: its /proc entry; elsewhere there's nothing to ask cheaply).
bool gone(qint64 pid)
{
#if defined(Q_OS_LINUX)
    return !QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid));
#else
    Q_UNUSED(pid)
    return true;
#endif
}

} // namespace

class TestGpuClient : public QObject
{
    Q_OBJECT

private slots:
    void init() { GpuClient::setWorkerForTests({}); }
    void cleanup()
    {
        GpuClient::setWorkerForTests({});
        qunsetenv("SYNBENCH_GPU_WORKER_ARGS");
    }

    void realWorkerOnTheNullBackend()
    {
        qputenv("SYNBENCH_GPU_WORKER_ARGS", "--api null --headless");
        GpuClient::setWorkerForTests(kWorker);
        QList<pr::Event> seen;
        const ClientOutcome o = GpuClient::run(spec(QStringLiteral("geometry:100:300")), {}, [&](const pr::Event &e) { seen.append(e); });
        QVERIFY2(o.status == Status::Ok, qPrintable(o.reason));
        QCOMPARE(o.results.size(), 1);
        QCOMPARE(o.results[0].workload, QStringLiteral("geometry"));
        QVERIFY(o.ready);
        QCOMPARE(o.ready->device, QStringLiteral("Null"));
        QCOMPARE(o.exitCode, 0);
        QVERIFY(seen.size() >= 3);
        QVERIFY(gone(o.pid));
    }

    void junkIsMalformed()
    {
        GpuClient::setWorkerForTests(kFake, {QStringLiteral("junk")});
        const ClientOutcome o = GpuClient::run(spec(QStringLiteral("geometry:100:300")), {}, {});
        QCOMPARE(o.status, Status::Failed);
        QVERIFY2(o.reason.startsWith(QLatin1StringView("the GPU worker sent a malformed line")), qPrintable(o.reason));
        QVERIFY(gone(o.pid));
    }

    void hangIsKilledAtTheTimeout()
    {
        GpuClient::setWorkerForTests(kFake, {QStringLiteral("hang")});
        ClientRunSpec s = spec(QStringLiteral("geometry:100:300"));
        s.timeout = std::chrono::milliseconds(500);
        QElapsedTimer t;
        t.start();
        const ClientOutcome o = GpuClient::run(s, {}, {});
        QVERIFY2(t.elapsed() < 1500, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(o.status, Status::Failed);
        QVERIFY2(o.reason.contains(QLatin1StringView("stopped responding")), qPrintable(o.reason));
        QVERIFY(gone(o.pid));
    }

    void crashIsFailed()
    {
        GpuClient::setWorkerForTests(kFake, {QStringLiteral("crash")});
        const ClientOutcome o = GpuClient::run(spec(QStringLiteral("geometry:100:300")), {}, {});
        QCOMPARE(o.status, Status::Failed);
        QVERIFY2(o.reason.contains(QLatin1StringView("exited with code")), qPrintable(o.reason));
    }

    void naIsNotAvailable()
    {
        GpuClient::setWorkerForTests(kFake, {QStringLiteral("na")});
        const ClientOutcome o = GpuClient::run(spec(QStringLiteral("geometry:100:300")), {}, {});
        QCOMPARE(o.status, Status::NotAvailable);
        QCOMPARE(o.reason, QStringLiteral("No Vulkan driver is installed"));
        QCOMPARE(o.exitCode, 5);
    }

    void cancelIsFast()
    {
        GpuClient::setWorkerForTests(kFake, {QStringLiteral("ok")});
        QElapsedTimer start, cancelAt;
        start.start();
        bool cancelling = false;
        const auto cancelled = [&] {
            if (!cancelling && start.elapsed() > 300) {
                cancelling = true;
                cancelAt.start();
            }
            return cancelling;
        };
        const ClientOutcome o = GpuClient::run(spec(QStringLiteral("geometry:1000:5000")), cancelled, {});
        QCOMPARE(o.status, Status::Cancelled);
        QVERIFY2(cancelAt.elapsed() <= 250, qPrintable(QString::number(cancelAt.elapsed())));
        QVERIFY(gone(o.pid));
    }

    // L5: a protocol error the worker explained before exiting 2 is the client's reason.
    void protocolErrorReasonIsCarried()
    {
        GpuClient::setWorkerForTests(kFake, {QStringLiteral("protocol-error")});
        const ClientOutcome o = GpuClient::run(spec(QStringLiteral("geometry:100:300")), {}, {});
        QCOMPARE(o.status, Status::Failed);
        QCOMPARE(o.reason, QStringLiteral("protocol: unknown command 'x'"));
        QCOMPARE(o.exitCode, 2);
    }

    void timeouts()
    {
        QCOMPARE(GpuClient::timeoutFor(*pr::parsePlan(QStringLiteral("geometry:5000:20000"), nullptr)), std::chrono::milliseconds(80000));
    }

    void environmentIsFromScratch()
    {
        qunsetenv("SYNBENCH_GPU_WORKER_ENV");  // the test run's own hook (software drivers) adds on purpose
        qputenv("LD_LIBRARY_PATH", "/tmp/planted");
        qputenv("QT_PLUGIN_PATH", "/tmp/planted");
        qputenv("VK_DRIVER_FILES", "/tmp/planted");
        const QProcessEnvironment env = GpuClient::baseEnvironment();
        QVERIFY(!env.contains(QStringLiteral("LD_LIBRARY_PATH")));
        QVERIFY(!env.contains(QStringLiteral("QT_PLUGIN_PATH")));
        QVERIFY(!env.contains(QStringLiteral("VK_DRIVER_FILES")));
#if defined(Q_OS_LINUX)
        QCOMPARE(env.value(QStringLiteral("VK_LOADER_LAYERS_DISABLE")), QStringLiteral("~implicit~"));
#endif
#if defined(Q_OS_WIN)
        // The worker must be able to start: the system folder and a PATH (a DLL search needs both).
        QVERIFY(env.contains(QStringLiteral("SystemRoot")));
        QVERIFY(env.value(QStringLiteral("PATH")).contains(QStringLiteral("System32"), Qt::CaseInsensitive));
#endif
        qunsetenv("LD_LIBRARY_PATH");
        qunsetenv("QT_PLUGIN_PATH");
        qunsetenv("VK_DRIVER_FILES");
    }

    void workerEnvHook()
    {
        qputenv("SYNBENCH_GPU_WORKER_ENV", "A=1|VK_DRIVER_FILES=/x/y");
        const QProcessEnvironment env = GpuClient::baseEnvironment();
        QCOMPARE(env.value(QStringLiteral("A")), QStringLiteral("1"));
        QCOMPARE(env.value(QStringLiteral("VK_DRIVER_FILES")), QStringLiteral("/x/y"));
        qunsetenv("SYNBENCH_GPU_WORKER_ENV");
    }

    // --- GpuSession (D12) ---

    void twoNullSessions()
    {
        qputenv("SYNBENCH_GPU_WORKER_ARGS", "--api null --headless");
        GpuClient::setWorkerForTests(kWorker);
        GpuSession a, b;
        QString err;
        QVERIFY2(a.start(QStringLiteral("vulkan"), QStringLiteral("0000:0000#0"), {}, &err), qPrintable(err));
        QVERIFY2(b.start(QStringLiteral("opengl"), QStringLiteral("0000:0000#0"), {}, &err), qPrintable(err));
        const ClientOutcome ra = a.waitReady(std::chrono::seconds(20), {}), rb = b.waitReady(std::chrono::seconds(20), {});
        QVERIFY2(ra.status == Status::Ok, qPrintable(ra.reason));
        QVERIFY2(rb.status == Status::Ok, qPrintable(rb.reason));
        QVERIFY(ra.ready);
        for (const auto &[session, workload] : {std::pair<GpuSession *, const char *>{&a, "geometry"}, {&b, "geometry"}, {&a, "quads"}}) {
            const ClientOutcome o = session->runWorkload({QString::fromLatin1(workload), 100, 300}, {}, {});
            QVERIFY2(o.status == Status::Ok, qPrintable(o.reason));
            QCOMPARE(o.results.size(), 1);
            QCOMPARE(o.results[0].workload, QString::fromLatin1(workload));
        }
        const qint64 pa = a.pid(), pb = b.pid();
        QElapsedTimer t;
        t.start();
        a.quit();
        b.quit();
        QVERIFY2(t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));
        QVERIFY(!a.alive() && !b.alive());
        QVERIFY(gone(pa) && gone(pb));
    }

    void crashAfterReady()
    {
        GpuClient::setWorkerForTests(kFake, {QStringLiteral("crash-after-ready")});
        GpuSession s;
        QString err;
        QVERIFY2(s.start(QStringLiteral("vulkan"), QStringLiteral("0000:0000#0"), {}, &err), qPrintable(err));
        QCOMPARE(s.waitReady(std::chrono::seconds(10), {}).status, Status::Ok);
        const ClientOutcome o = s.runWorkload({QStringLiteral("geometry"), 100, 300}, {}, {});
        QCOMPARE(o.status, Status::Failed);
        QVERIFY2(o.reason.contains(QLatin1StringView("exited with code")), qPrintable(o.reason));
        // The next workload on a dead session fails at once, without a hang.
        QCOMPARE(s.runWorkload({QStringLiteral("shading"), 100, 300}, {}, {}).status, Status::Failed);
    }

    void naSession()
    {
        GpuClient::setWorkerForTests(kFake, {QStringLiteral("na")});
        GpuSession s;
        QString err;
        QVERIFY(s.start(QStringLiteral("vulkan"), QStringLiteral("0000:0000#0"), {}, &err));
        const ClientOutcome o = s.waitReady(std::chrono::seconds(10), {});
        QCOMPARE(o.status, Status::NotAvailable);
        QCOMPARE(o.reason, QStringLiteral("No Vulkan driver is installed"));
    }

    void cancelDuringAWorkload()
    {
        GpuClient::setWorkerForTests(kFake, {QStringLiteral("ok")});
        auto s = std::make_unique<GpuSession>();
        QString err;
        QVERIFY(s->start(QStringLiteral("vulkan"), QStringLiteral("0000:0000#0"), {}, &err));
        QCOMPARE(s->waitReady(std::chrono::seconds(10), {}).status, Status::Ok);
        const qint64 pid = s->pid();
        QElapsedTimer start, cancelAt;
        start.start();
        bool cancelling = false;
        const auto cancelled = [&] {
            if (!cancelling && start.elapsed() > 300) {
                cancelling = true;
                cancelAt.start();
            }
            return cancelling;
        };
        const ClientOutcome o = s->runWorkload({QStringLiteral("geometry"), 1000, 5000}, cancelled, {});
        QCOMPARE(o.status, Status::Cancelled);
        QVERIFY2(cancelAt.elapsed() <= 250, qPrintable(QString::number(cancelAt.elapsed())));
        QVERIFY(!s->alive());
        QVERIFY(gone(pid));
    }

    void destroyingASessionKillsTheWorker()
    {
        GpuClient::setWorkerForTests(kFake, {QStringLiteral("hang")});
        qint64 pid = 0;
        {
            GpuSession s;
            QString err;
            QVERIFY(s.start(QStringLiteral("vulkan"), QStringLiteral("0000:0000#0"), {}, &err));
            QCOMPARE(s.waitReady(std::chrono::seconds(10), {}).status, Status::Ok);
            pid = s.pid();
            QVERIFY(s.alive());
        }
        QVERIFY(gone(pid));
    }
};

QTEST_GUILESS_MAIN(TestGpuClient)
#include "tst_gpu_client.moc"
