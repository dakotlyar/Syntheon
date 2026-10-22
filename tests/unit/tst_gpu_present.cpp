// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU worker with a real window and swapchain (Phase 4a task 10: Vulkan
// on lavapipe through X11, with validation). The worker always runs inside
// its own invisible X server (xvfb-run around the worker, not just around
// ctest): every unit test runs with QT_QPA_PLATFORM=offscreen and the
// session's DISPLAY, so ctest alone can't tell xvfb from Dave's desktop, and
// a test window must never appear there. Skipped without xvfb-run or lavapipe.
#include "gpu_worker_process.h"

#include <QStandardPaths>
#include <QTest>

using namespace sb::gpu::testing;

namespace {

const QString kWorker = QStringLiteral(SB_GPU_WORKER);

} // namespace

class TestGpuPresent : public QObject
{
    Q_OBJECT

    QProcessEnvironment m_env;
    QString m_xvfbRun;

    // xvfb-run -a -s "-screen 0 1920x1080x24" synbench-gpu --run --api vulkan ... (A-X's screen).
    static QStringList vulkanArgs()
    {
        return {QStringLiteral("-a"), QStringLiteral("-s"), QStringLiteral("-screen 0 1920x1080x24"), kWorker,
                QStringLiteral("--run"), QStringLiteral("--api"), QStringLiteral("vulkan"), QStringLiteral("--gpu"), kLavapipeId,
                QStringLiteral("--allow-software"), QStringLiteral("--render-size"), QStringLiteral("320x180"),
                QStringLiteral("--content-scale"), QStringLiteral("64"), QStringLiteral("--validation")};
    }

private slots:
    void initTestCase()
    {
#if !defined(Q_OS_LINUX)
        QSKIP("the windowed Vulkan case runs on Linux (lavapipe under xvfb)");
#endif
        m_xvfbRun = QStandardPaths::findExecutable(QStringLiteral("xvfb-run"));
        if (m_xvfbRun.isEmpty())
            SB_SKIP_OR_FAIL("xvfb-run isn't installed (the windowed cases never use the real display)");
        if (lavapipeIcd().isEmpty())
            SB_SKIP_OR_FAIL("lavapipe (the software Vulkan driver) isn't installed");
        // X11 (xcb) on xvfb-run's DISPLAY, never the session's Wayland.
        m_env = QProcessEnvironment::systemEnvironment();
        m_env.remove(QStringLiteral("WAYLAND_DISPLAY"));
        m_env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("xcb"));
        addSoftwareVulkan(m_env);
    }

    // A 1 s window run with a swapchain: vsync off (IMMEDIATE or MAILBOX), no
    // validation error.
    void vulkanPresent()
    {
        WorkerProcess w(m_xvfbRun, vulkanArgs() << QStringLiteral("--plan") << QStringLiteral("geometry:200:1000"), m_env);
        QVERIFY(w.started());
        const int code = w.finish();
        QVERIFY2(code == 0, qPrintable(QStringLiteral("exit %1: %2 %3").arg(code).arg(errors(w.events()), w.stderrText())));
        const QList<pr::ResultEvent> rs = results(w.events());
        QCOMPARE(rs.size(), 1);
        QVERIFY2(rs[0].present == QLatin1StringView("IMMEDIATE") || rs[0].present == QLatin1StringView("MAILBOX"), qPrintable(rs[0].present));
        QVERIFY(rs[0].frames >= 3);
        const auto ready = std::get<pr::ReadyEvent>(w.events().first());
        QVERIFY(ready.windowW >= 640 && ready.windowH >= 360);
        // D1: the result records the screen the window actually used.
        QVERIFY(!rs[0].screen.isEmpty());
        QCOMPARE(rs[0].windowW, ready.windowW);
        QCOMPARE(rs[0].renderW, 320);
        QVERIFY2(!w.stderrText().contains(QLatin1StringView("vulkan:")), qPrintable(w.stderrText()));
    }

    // Serve mode with a window: two commands, so the swapchain is released
    // (suspend, hide) and made again (show, resume) in between.
    void vulkanSuspendResume()
    {
        WorkerProcess w(m_xvfbRun, vulkanArgs() << QStringLiteral("--serve"), m_env);
        QVERIFY(w.started());
        QVERIFY2(w.waitFor([](const pr::Event &e) { return is<pr::ReadyEvent>(e); }), qPrintable(w.problem()));
        for (const char *cmd : {"run geometry:100:300", "run sprites:100:300"}) {
            w.send(cmd);
            QVERIFY2(w.waitForIdle(), qPrintable(w.problem()));
        }
        w.send("quit");
        QCOMPARE(w.finish(), 0);
        QVERIFY2(errors(w.events()).isEmpty(), qPrintable(errors(w.events())));
        const QList<pr::ResultEvent> rs = results(w.events());
        QCOMPARE(rs.size(), 2);
        for (const pr::ResultEvent &r : rs) {
            QVERIFY(!r.present.isEmpty());
            QVERIFY(r.frames >= 3);
        }
        QVERIFY(is<pr::DoneEvent>(w.events().last()));
        QVERIFY2(!w.stderrText().contains(QLatin1StringView("vulkan:")), qPrintable(w.stderrText()));
    }
    // D1: the window can land on a screen the plan wasn't made for. A (pretend) bigger screen at
    // the same scale grows the window to the full 1920x1080 before the measurement starts, and the
    // result says so; the run is not an error.
    void windowFollowsTheScreenItLandsOn()
    {
        QProcessEnvironment env = m_env;
        env.insert(QStringLiteral("SYNBENCH_GPU_FAKE_SCREEN"), QStringLiteral("3440x1400@1"));
        WorkerProcess w(m_xvfbRun, vulkanArgs() << QStringLiteral("--plan") << QStringLiteral("geometry:200:500"), env);
        QVERIFY(w.started());
        const int code = w.finish();
        QVERIFY2(code == 0, qPrintable(QStringLiteral("exit %1: %2 %3").arg(code).arg(errors(w.events()), w.stderrText())));
        const auto ready = std::get<pr::ReadyEvent>(w.events().first());
        QVERIFY(ready.fit);  // xvfb's 1920x1080 screen: the first plan is scaled to fit
        const QList<pr::ResultEvent> rs = results(w.events());
        QCOMPARE(rs.size(), 1);
        QVERIFY(rs[0].frames >= 3);
        QCOMPARE(rs[0].windowW, 1920);
        QCOMPARE(rs[0].windowH, 1080);
        QVERIFY(!rs[0].fit);
    }

    // The 610M under XWayland: a swapchain that can't be made is N/A, not an Error (exit 5, na).
    void swapchainFailureIsNotAvailable()
    {
        QProcessEnvironment env = m_env;
        env.insert(QStringLiteral("SYNBENCH_GPU_FAIL_SWAPCHAIN"), QStringLiteral("1"));
        WorkerProcess w(m_xvfbRun, vulkanArgs() << QStringLiteral("--plan") << QStringLiteral("geometry:100:300"), env);
        QVERIFY(w.started());
        QCOMPARE(w.finish(), 5);
        QVERIFY(results(w.events()).isEmpty());
        bool na = false;
        for (const pr::Event &e : w.events()) {
            if (const auto *err = std::get_if<pr::ErrorEvent>(&e)) {
                na = err->na;
                QCOMPARE(err->reason, QStringLiteral("this GPU can't present to the test window on this display system"));
            }
        }
        QVERIFY(na);
    }
    // M1: serve mode, the window lands on a screen too small for it (pretend): the run command
    // answers with an N/A error (na:true), like at start, then idle; the worker stays usable.
    void tooSmallAfterReplanIsNotAvailable()
    {
        QProcessEnvironment env = m_env;
        env.insert(QStringLiteral("SYNBENCH_GPU_FAKE_SCREEN"), QStringLiteral("500x300@1"));
        WorkerProcess w(m_xvfbRun, vulkanArgs() << QStringLiteral("--serve"), env);
        QVERIFY(w.started());
        QVERIFY2(w.waitFor([](const pr::Event &e) { return is<pr::ReadyEvent>(e); }), qPrintable(w.problem()));
        w.send("run geometry:100:300");
        QVERIFY2(w.waitForIdle(), qPrintable(w.problem()));
        w.send("quit");
        QCOMPARE(w.finish(), 0);
        bool na = false;
        for (const pr::Event &e : w.events()) {
            if (const auto *err = std::get_if<pr::ErrorEvent>(&e)) {
                na = err->na;
                QCOMPARE(err->reason, QStringLiteral("screen too small for the test window"));
            }
        }
        QVERIFY(na);
        QVERIFY(results(w.events()).isEmpty());
    }

    // L8: a swapchain that can't be made is N/A only on an API's first resume; later it is an Error.
    void laterSwapchainFailureIsAnError()
    {
        QProcessEnvironment env = m_env;
        env.insert(QStringLiteral("SYNBENCH_GPU_FAIL_SWAPCHAIN"), QStringLiteral("later"));
        WorkerProcess w(m_xvfbRun, vulkanArgs() << QStringLiteral("--serve"), env);
        QVERIFY(w.started());
        QVERIFY2(w.waitFor([](const pr::Event &e) { return is<pr::ReadyEvent>(e); }), qPrintable(w.problem()));
        for (const char *cmd : {"run geometry:100:300", "run sprites:100:300"}) {
            w.send(cmd);
            QVERIFY2(w.waitForIdle(), qPrintable(w.problem()));
        }
        w.send("quit");
        QCOMPARE(w.finish(), 0);
        QCOMPARE(results(w.events()).size(), 1);  // the first workload ran
        int errors = 0;
        for (const pr::Event &e : w.events()) {
            if (const auto *err = std::get_if<pr::ErrorEvent>(&e)) {
                ++errors;
                QVERIFY(!err->na);
                QVERIFY2(err->reason.contains(QLatin1StringView("again after the first workload")), qPrintable(err->reason));
            }
        }
        QCOMPARE(errors, 1);
    }
    // Under xvfb-run (which merges stderr into stdout on Debian/Ubuntu) the old-layer note must not
    // break the protocol reading: the harness takes "synbench-gpu: ..." lines as stderr.
    void oldLayerNoteDoesNotBreakTheProtocol()
    {
        QProcessEnvironment env = m_env;
        env.insert(QStringLiteral("SYNBENCH_GPU_FAKE_LAYER_SPEC"), QStringLiteral("1.3.204"));
        WorkerProcess w(m_xvfbRun, vulkanArgs() << QStringLiteral("--plan") << QStringLiteral("geometry:100:300"), env);
        QVERIFY(w.started());
        QCOMPARE(w.finish(), 0);
        QVERIFY2(!w.problem().contains(QLatin1StringView("malformed")), qPrintable(w.problem()));  // every stdout line parsed
        QCOMPARE(results(w.events()).size(), 1);
        QVERIFY(w.stderrText().contains(QLatin1StringView("sync validation skipped")));
    }
    // DEF-1: KWin reports the window's real screen only after its first frames. A change of screen
    // or scale during the warm-up is not an error: the worker re-plans from the window's own
    // (fractional) ratio, resizes, restarts the warm-up, and the result shows the final placement.
    void screenChangeDuringWarmupReplans()
    {
        QProcessEnvironment env = m_env;
        env.insert(QStringLiteral("SYNBENCH_GPU_FAKE_SCREEN"), QStringLiteral("1280x800@1;3440x1400@1.25"));
        WorkerProcess w(m_xvfbRun, vulkanArgs() << QStringLiteral("--plan") << QStringLiteral("geometry:300:500"), env);
        QVERIFY(w.started());
        const int code = w.finish();
        QVERIFY2(code == 0, qPrintable(QStringLiteral("exit %1: %2 %3").arg(code).arg(errors(w.events()), w.stderrText())));
        const QList<pr::ResultEvent> rs = results(w.events());
        QCOMPARE(rs.size(), 1);
        QVERIFY(rs[0].frames >= 3);
        QCOMPARE(rs[0].windowW, 1920);  // the second screen's plan, not the first's
        QCOMPARE(rs[0].windowH, 1080);
        QVERIFY(!rs[0].fit);
        QCOMPARE(rs[0].scale, 1.25);    // fractional, as the window reports it
    }

    // DEF-2 and "only a change during the measurement is an error": the real hide path runs, and the
    // reason is the window's own ("moved"), not "hidden or minimised".
    void moveDuringMeasurementIsReportedAsMoved()
    {
        QProcessEnvironment env = m_env;
        env.insert(QStringLiteral("SYNBENCH_GPU_FAKE_MOVE"), QStringLiteral("measure"));
        WorkerProcess w(m_xvfbRun, vulkanArgs() << QStringLiteral("--plan") << QStringLiteral("geometry:100:500"), env);
        QVERIFY(w.started());
        QCOMPARE(w.finish(), 1);
        QVERIFY(results(w.events()).isEmpty());
        QCOMPARE(errors(w.events()), QStringLiteral("the test window moved to another screen during the measurement"));
    }

    // At most 3 re-plans, then an Error.
    void windowThatKeepsMovingIsAnError()
    {
        QProcessEnvironment env = m_env;
        env.insert(QStringLiteral("SYNBENCH_GPU_FAKE_SCREEN"),
                   QStringLiteral("1280x800@1;1281x800@1;1282x800@1;1283x800@1;1284x800@1"));
        WorkerProcess w(m_xvfbRun, vulkanArgs() << QStringLiteral("--plan") << QStringLiteral("geometry:300:500"), env);
        QVERIFY(w.started());
        QCOMPARE(w.finish(), 1);
        QVERIFY(results(w.events()).isEmpty());
        QCOMPARE(errors(w.events()), QStringLiteral("the test window kept moving between screens"));
    }
};

QTEST_GUILESS_MAIN(TestGpuPresent)
#include "tst_gpu_present.moc"
