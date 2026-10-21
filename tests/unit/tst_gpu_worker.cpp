// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU worker (Phase 4a task 9): the built synbench-gpu with the null
// backend, through QProcess: the --plan and --serve protocols, exit codes,
// stop and the stdin lifeline; plus the block signature. No GPU.
#include "gpu_worker_process.h"
#include "tests/gpu/model/protocol.h"
#include "tests/gpu/worker/signature.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QTest>

#include <functional>
#include <variant>

namespace pr = sb::gpu::protocol;

namespace {

const QString kWorker = QStringLiteral(SB_GPU_WORKER);

template <class T>
bool is(const pr::Event &e)
{
    return std::holds_alternative<T>(e);
}

// One worker process and the events it has sent so far.
class Worker
{
public:
    explicit Worker(const QStringList &args, bool offscreen = false)
    {
        if (offscreen) {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
            m_p.setProcessEnvironment(env);
        }
        m_p.start(kWorker, args, QIODevice::ReadWrite);
        m_started = m_p.waitForStarted(10000);
    }
    ~Worker()
    {
        if (m_p.state() != QProcess::NotRunning) {
            m_p.kill();
            m_p.waitForFinished(5000);
        }
    }
    bool started() const { return m_started; }
    QProcess &process() { return m_p; }
    const QList<pr::Event> &events() const { return m_events; }
    const QString &problem() const { return m_problem; }

    // Reads events until one matches (true), the process ends or the time is up.
    bool waitFor(const std::function<bool(const pr::Event &)> &match, int timeoutMs = 20000)
    {
        QElapsedTimer t;
        t.start();
        for (;;) {
            while (m_p.canReadLine()) {
                const QByteArray line = m_p.readLine().trimmed();
                if (line.isEmpty())
                    continue;
                QString err;
                const auto e = pr::parseLine(line, &err);
                if (!e) {
                    m_problem = QStringLiteral("malformed line '%1': %2").arg(QString::fromUtf8(line), err);
                    return false;
                }
                m_events.append(*e);
                if (match(*e))
                    return true;
            }
            if (t.elapsed() > timeoutMs) {
                m_problem = QStringLiteral("timed out");
                return false;
            }
            if (m_p.state() == QProcess::NotRunning && !m_p.canReadLine() && m_p.bytesAvailable() == 0) {
                m_problem = QStringLiteral("the worker ended (exit %1): %2")
                                .arg(m_p.exitCode())
                                .arg(stderrText());
                return false;
            }
            m_p.waitForReadyRead(50);
        }
    }
    bool waitForStage(const char *phase)
    {
        return waitFor([phase](const pr::Event &e) {
            return is<pr::StageEvent>(e) && std::get<pr::StageEvent>(e).phase == QLatin1StringView(phase);
        });
    }
    bool waitForIdle()
    {
        return waitFor([](const pr::Event &e) { return is<pr::IdleEvent>(e); });
    }
    // Waits for the exit (draining stdout into events); the exit code, or -1.
    int finish(int timeoutMs = 20000)
    {
        if (!m_p.waitForFinished(timeoutMs))
            return -1;
        waitFor([](const pr::Event &) { return false; }, 0);
        return m_p.exitStatus() == QProcess::NormalExit ? m_p.exitCode() : -1;
    }
    void send(const QByteArray &line) { m_p.write(line + '\n'); m_p.waitForBytesWritten(1000); }
    // Everything on stderr so far.
    QString stderrText()
    {
        m_stderr += QString::fromUtf8(m_p.readAllStandardError());
        return m_stderr;
    }

private:
    QProcess m_p;
    bool m_started = false;
    QList<pr::Event> m_events;
    QString m_problem, m_stderr;
};

QStringList headlessRun(const QString &plan)
{
    return {QStringLiteral("--run"), QStringLiteral("--api"), QStringLiteral("null"), QStringLiteral("--headless"),
            QStringLiteral("--plan"), plan};
}

QStringList headlessServe()
{
    return {QStringLiteral("--run"), QStringLiteral("--serve"), QStringLiteral("--api"), QStringLiteral("null"),
            QStringLiteral("--headless"), QStringLiteral("--content-scale"), QStringLiteral("64")};
}

// The kinds of events, in order, compressing repeated stages: "ready stage result done".
QString shape(const QList<pr::Event> &events)
{
    QStringList out;
    for (const pr::Event &e : events) {
        const char *name = is<pr::ReadyEvent>(e)    ? "ready"
                           : is<pr::StageEvent>(e)  ? "stage"
                           : is<pr::ResultEvent>(e) ? "result"
                           : is<pr::ErrorEvent>(e)  ? "error"
                           : is<pr::IdleEvent>(e)   ? "idle"
                           : is<pr::DoneEvent>(e)   ? "done"
                                                    : "other";
        if (out.isEmpty() || out.last() != QLatin1StringView(name) || out.last() != QLatin1StringView("stage"))
            out << QString::fromLatin1(name);
    }
    return out.join(QLatin1Char(' '));
}

QList<pr::ResultEvent> results(const QList<pr::Event> &events)
{
    QList<pr::ResultEvent> out;
    for (const pr::Event &e : events) {
        if (is<pr::ResultEvent>(e))
            out << std::get<pr::ResultEvent>(e);
    }
    return out;
}

} // namespace

class TestGpuWorker : public QObject
{
    Q_OBJECT

private slots:

    // Task 13: --probe lists the Vulkan devices merged with the DRM cards, one
    // gpu event each, then done. It opens no window (offscreen platform).
    // Software rasterisers only appear with --allow-software (P12).
    void probeListsGpus()
    {
        if (sb::gpu::testing::lavapipeIcd().isEmpty())
            SB_SKIP_OR_FAIL("lavapipe (the software Vulkan driver) isn't installed");
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        sb::gpu::testing::addSoftwareVulkan(env);  // only lavapipe: the list is the same on every machine
        for (const bool software : {false, true}) {
            QProcess p;
            p.setProcessEnvironment(env);
            p.start(kWorker, software ? QStringList{QStringLiteral("--probe"), QStringLiteral("--allow-software")}
                                      : QStringList{QStringLiteral("--probe")});
            QVERIFY(p.waitForStarted(10000));
            QVERIFY(p.waitForFinished(20000));
            QCOMPARE(p.exitCode(), 0);
            QList<pr::GpuEvent> gpus;
            bool done = false;
            for (const QByteArray &line : p.readAllStandardOutput().split('\n')) {
                if (line.trimmed().isEmpty())
                    continue;
                QString err;
                const auto e = pr::parseLine(line, &err);
                QVERIFY2(e, qPrintable(err));
                if (const auto *g = std::get_if<pr::GpuEvent>(&*e))
                    gpus.append(*g);
                done = std::holds_alternative<pr::DoneEvent>(*e);
            }
            QVERIFY(done);  // done is the last line
            // The machine's own DRM cards are listed too (without a Vulkan driver here:
            // only lavapipe is visible); what the flag decides is the software entry.
            const sb::gpu::GpuEntry *lvp = nullptr;
            for (const pr::GpuEvent &g : std::as_const(gpus)) {
                if (g.gpu.id == sb::gpu::testing::kLavapipeId)
                    lvp = &g.gpu;
            }
            if (!software) {
                QVERIFY2(!lvp, "lavapipe is a software rasteriser: not offered");
                for (const pr::GpuEvent &g : std::as_const(gpus))
                    QVERIFY(g.gpu.kind != sb::gpu::GpuKind::Software);
                continue;
            }
            QVERIFY(lvp);
            QCOMPARE(lvp->kind, sb::gpu::GpuKind::Software);
            QVERIFY(lvp->apis.value(QStringLiteral("vulkan")).ok);
            QCOMPARE(lvp->apis.value(QStringLiteral("dx12")).reason, QStringLiteral("DirectX is Windows only"));
        }
    }

    void planRun()
    {
        Worker w(headlessRun(QStringLiteral("geometry:100:300")));
        QVERIFY(w.started());
        const int code = w.finish();
        QVERIFY2(code == 0, qPrintable(w.stderrText()));
        QCOMPARE(shape(w.events()), QStringLiteral("ready stage result done"));
        const auto ready = std::get<pr::ReadyEvent>(w.events().first());
        QCOMPARE(ready.device, QStringLiteral("Null"));
        QCOMPARE(ready.api, QStringLiteral("null"));
        QCOMPARE(ready.present, QStringLiteral("none"));
        QCOMPARE(ready.renderW, 1920);
        QCOMPARE(ready.renderH, 1080);
        QCOMPARE(ready.windowW, 0);  // headless
        QVERIFY(ready.gpuTimestamps);
        const QList<pr::ResultEvent> r = results(w.events());
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].workload, QStringLiteral("geometry"));
        QVERIFY2(r[0].frames >= 30 && r[0].frames <= 300, qPrintable(QString::number(r[0].frames)));
        QVERIFY(r[0].gpuMs);
        QCOMPARE(*r[0].gpuMs, 0.5);
        QVERIFY(r[0].seconds >= 0.3 && r[0].seconds < 1.0);
        QVERIFY(r[0].fps > 0 && r[0].ftP1 <= r[0].ftP50 && r[0].ftP50 <= r[0].ftP99);
        QVERIFY(!r[0].series.isEmpty());
        QCOMPARE(r[0].signature.size(), qsizetype(sb::gpu::kSignatureBytes));
        // Every phase was announced, in order.
        QStringList phases;
        for (const pr::Event &e : w.events()) {
            if (is<pr::StageEvent>(e) && !phases.contains(std::get<pr::StageEvent>(e).phase))
                phases << std::get<pr::StageEvent>(e).phase;
        }
        QCOMPARE(phases, (QStringList{QStringLiteral("load"), QStringLiteral("warmup"), QStringLiteral("measure"),
                                      QStringLiteral("verify")}));
    }

    void selfTest()
    {
        QProcess p;
        p.start(kWorker, {QStringLiteral("--self-test")});
        QVERIFY(p.waitForFinished(60000));
        QVERIFY2(p.exitCode() == 0, qPrintable(QString::fromUtf8(p.readAllStandardError())));
    }

    void badArguments()
    {
        QProcess p;
        p.start(kWorker, {QStringLiteral("--run"), QStringLiteral("--api"), QStringLiteral("metal")});
        QVERIFY(p.waitForFinished(10000));
        QCOMPARE(p.exitCode(), 2);

        Worker w(headlessRun(QStringLiteral("geometry:100")));
        QCOMPARE(w.finish(), 2);
        const QString err = w.stderrText();
        QVERIFY2(err.contains(QStringLiteral("bad plan item 'geometry:100'")), qPrintable(err));

        Worker unknown(headlessRun(QStringLiteral("teapot:100:100")));
        QCOMPARE(unknown.finish(), 2);
    }

    void stdinClosed()
    {
        Worker w(headlessRun(QStringLiteral("geometry:100:60000")));
        QVERIFY2(w.waitForStage("measure"), qPrintable(w.problem()));
        QElapsedTimer t;
        t.start();
        w.process().closeWriteChannel();
        QCOMPARE(w.finish(1000), 3);
        QVERIFY(t.elapsed() < 1000);
    }

    void stopDuringMeasure()
    {
        Worker w(headlessRun(QStringLiteral("geometry:100:60000")));
        QVERIFY2(w.waitForStage("measure"), qPrintable(w.problem()));
        QElapsedTimer t;
        t.start();
        w.send("stop");
        QCOMPARE(w.finish(1000), 4);
        QVERIFY2(t.elapsed() <= 250, qPrintable(QString::number(t.elapsed())));
    }

    // 32x18: left half (255,0,0), right half (0,0,255) -> 2x2-pixel blocks,
    // bx < 8 red, the rest blue.
    void blockSignature()
    {
        std::vector<std::uint8_t> img(32 * 18 * 4);
        for (int y = 0; y < 18; ++y) {
            for (int x = 0; x < 32; ++x) {
                std::uint8_t *p = &img[std::size_t(y * 32 + x) * 4];
                p[0] = x < 16 ? 255 : 0;
                p[1] = 0;
                p[2] = x < 16 ? 0 : 255;
                p[3] = 255;
            }
        }
        const std::vector<std::uint8_t> sig = sb::gpu::blockSignature(img.data(), 32, 18);
        QCOMPARE(sig.size(), std::size_t(432));
        for (int by = 0; by < 9; ++by) {
            for (int bx = 0; bx < 16; ++bx) {
                const std::uint8_t *b = &sig[std::size_t(by * 16 + bx) * 3];
                const bool red = bx < 8;
                QCOMPARE(int(b[0]), red ? 255 : 0);
                QCOMPARE(int(b[1]), 0);
                QCOMPARE(int(b[2]), red ? 0 : 255);
            }
        }
        // Rounding: a block of (0, 1) means rounds half up, (0 + 1 + 1) / 2 = 1.
        std::vector<std::uint8_t> two(16 * 9 * 2 * 4, 0);
        for (std::size_t i = 0; i < two.size(); i += 8)
            two[i] = 1;  // every other pixel's red is 1 (the blocks are 2 x 1)
        QCOMPARE(int(sb::gpu::blockSignature(two.data(), 32, 9)[0]), 1);
    }

    // Serve mode (D12): two run commands, each answered by its result and
    // idle, then quit; the backend was resumed and suspended once per run.
    void serveTwoRuns()
    {
        Worker w(headlessServe());
        QVERIFY2(w.waitFor([](const pr::Event &e) { return is<pr::ReadyEvent>(e); }), qPrintable(w.problem()));
        w.send("run geometry:100:300");
        QVERIFY2(w.waitForIdle(), qPrintable(w.problem()));
        w.send("run quads:100:200");
        QVERIFY2(w.waitForIdle(), qPrintable(w.problem()));
        w.send("quit");
        QCOMPARE(w.finish(), 0);
        QCOMPARE(shape(w.events()), QStringLiteral("ready stage result idle stage result idle done"));
        const QList<pr::ResultEvent> r = results(w.events());
        QCOMPARE(r[0].workload, QStringLiteral("geometry"));
        QCOMPARE(r[1].workload, QStringLiteral("quads"));
        QVERIFY(r[1].frames >= 20 && r[1].frames <= 200);
        const QString err = w.stderrText();
        QVERIFY2(err.contains(QStringLiteral("null backend: resume 2, suspend 2")), qPrintable(err));
    }

    void serveRunWhileBusy()
    {
        Worker w(headlessServe());
        QVERIFY2(w.waitFor([](const pr::Event &e) { return is<pr::ReadyEvent>(e); }), qPrintable(w.problem()));
        w.send("run geometry:100:2000\nrun quads:100:200");
        QCOMPARE(w.finish(), 2);
        QVERIFY(!w.events().isEmpty() && is<pr::ErrorEvent>(w.events().last()));
        QCOMPARE(std::get<pr::ErrorEvent>(w.events().last()).reason, QStringLiteral("protocol: run while busy"));

        Worker bad(headlessServe());
        QVERIFY2(bad.waitFor([](const pr::Event &e) { return is<pr::ReadyEvent>(e); }), qPrintable(bad.problem()));
        bad.send("draw everything");
        QCOMPARE(bad.finish(), 2);
        QCOMPARE(std::get<pr::ErrorEvent>(bad.events().last()).reason, QStringLiteral("protocol: unknown command 'draw everything'"));
    }

    void serveStop()
    {
        Worker w(headlessServe());
        QVERIFY2(w.waitFor([](const pr::Event &e) { return is<pr::ReadyEvent>(e); }), qPrintable(w.problem()));
        w.send("run geometry:100:60000");
        QVERIFY2(w.waitForStage("measure"), qPrintable(w.problem()));
        QElapsedTimer t;
        t.start();
        w.send("stop");
        QCOMPARE(w.finish(1000), 4);
        QVERIFY2(t.elapsed() <= 250, qPrintable(QString::number(t.elapsed())));
        QVERIFY(results(w.events()).isEmpty());
    }

    void serveEofWhileIdle()
    {
        Worker w(headlessServe());
        QVERIFY2(w.waitFor([](const pr::Event &e) { return is<pr::ReadyEvent>(e); }), qPrintable(w.problem()));
        w.send("run quads:50:100");
        QVERIFY2(w.waitForIdle(), qPrintable(w.problem()));
        QElapsedTimer t;
        t.start();
        w.process().closeWriteChannel();
        QCOMPARE(w.finish(1000), 3);
        QVERIFY(t.elapsed() < 1000);
    }

    // The window path (show, first expose, resume ... suspend, hide) on Qt's
    // offscreen platform, so no display is needed.
    void serveWindowed()
    {
        Worker w({QStringLiteral("--run"), QStringLiteral("--serve"), QStringLiteral("--api"), QStringLiteral("null"),
                  QStringLiteral("--render-size"), QStringLiteral("320x180"), QStringLiteral("--content-scale"), QStringLiteral("64")},
                 true);
        QVERIFY2(w.waitFor([](const pr::Event &e) { return is<pr::ReadyEvent>(e); }), qPrintable(w.problem()));
        const auto ready = std::get<pr::ReadyEvent>(w.events().first());
        QCOMPARE(ready.renderW, 320);
        QCOMPARE(ready.renderH, 180);
        QVERIFY(ready.windowW >= 640 && ready.windowH >= 360);
        QCOMPARE(ready.windowW * 9, ready.windowH * 16);
        for (const char *cmd : {"run shading:50:150", "run sprites:50:150"}) {
            w.send(cmd);
            QVERIFY2(w.waitForIdle(), qPrintable(w.problem()));
        }
        w.send("quit");
        QCOMPARE(w.finish(), 0);
        QCOMPARE(shape(w.events()), QStringLiteral("ready stage result idle stage result idle done"));
    }
};

QTEST_GUILESS_MAIN(TestGpuWorker)
#include "tst_gpu_worker.moc"
