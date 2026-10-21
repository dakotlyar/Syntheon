// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import SynBench

// The Progress screen on a real (fast) stub run.
TestCase {
    id: test
    name: "ProgressScreen"
    width: 1000
    height: 900
    visible: true
    when: windowShown

    Component { id: screenComponent; ProgressScreen { width: 1000; height: 900 } }
    Component { id: spyComponent; SignalSpy {} }

    function initTestCase() {
        tryVerify(function() { return !RunController.probing }, 20000)
        tryVerify(function() { return AppController.authStateName === "ready" }, 10000)
    }

    // The first test in the process: nothing has run yet.
    function test_1_emptyStateBeforeAnyRun() {
        const screen = createTemporaryObject(screenComponent, test)
        verify(!RunController.hasRun)
        verify(findChild(screen, "emptyState").visible)
        verify(!findChild(screen, "queueBox").visible)
        verify(!findChild(screen, "stopRun").visible)
        verify(!findChild(screen, "viewResults").visible)
        compare(screen.title, "Progress")
    }

    function test_2_aRunShowsQueueLogAndStop() {
        RunController.clearSelection()
        RunController.toggle("storage")
        RunController.toggle("ai")  // stubs only (Phase 3 N6: RAM is real now)
        const screen = createTemporaryObject(screenComponent, test)
        const finished = createTemporaryObject(spyComponent, test, { target: RunController, signalName: "runFinished" })
        verify(RunController.start())
        waitForRendering(screen)

        verify(RunController.running)
        verify(!findChild(screen, "emptyState").visible)
        verify(findChild(screen, "queueBox").visible)
        verify(findChild(screen, "stopRun").visible)
        compare(findChild(screen, "viewResults").text, "View results so far")
        compare(screen.title, "Running 2 tests")
        verify(findChild(screen, "queue-storage") !== null)
        verify(findChild(screen, "queue-ai") !== null)
        compare(AppController.section, "progress")

        // The log fills while it runs.
        tryVerify(function() { return RunController.log.rowCount() > 2 }, 20000)
        verify(findChild(screen, "logView").count > 0)

        // Zeb #4: the test name has its own column and isn't repeated in the message.
        const view = findChild(screen, "logView")
        waitForRendering(screen)
        let checked = 0
        for (let i = 0; i < view.contentItem.children.length; ++i) {
            const row = view.contentItem.children[i]
            const name = findChild(row, "logTest")
            const body = findChild(row, "logBody")
            if (!name || !body || name.text.length === 0)
                continue
            verify(body.x >= name.x + name.width, "message overlaps the name column")
            verify(body.text.indexOf(name.text + ":") !== 0, "the message repeats the test name: " + body.text)
            ++checked
        }
        verify(checked > 0, "no log line with a test name was rendered")

        finished.wait(60000)
        compare(finished.count, 1)
        verify(!RunController.running)
        verify(!findChild(screen, "stopRun").visible)
        compare(findChild(screen, "viewResults").text, "View results")
        compare(screen.title, "Run of 2 tests")
        compare(findChild(findChild(screen, "queue-storage"), "state").text, "✓ done")
        compare(findChild(findChild(screen, "queue-ai"), "state").text, "✓ done")
        verify(findChild(findChild(screen, "queue-storage"), "stage").text.indexOf("Done") === 0)
        verify(!findChild(screen, "restartBanner").visible)
    }

    function test_3_viewResultsNavigates() {
        const screen = createTemporaryObject(screenComponent, test)
        AppController.section = "progress"
        mouseClick(findChild(screen, "viewResults"))
        compare(AppController.section, "results")
        AppController.section = "run"
    }

    function test_4_stopRun() {
        // Stop mid-run: the run is saved as Stopped (D3) and the row says so.
        RunController.clearSelection()
        RunController.toggle("storage")
        RunController.toggle("ai")
        const screen = createTemporaryObject(screenComponent, test)
        const finished = createTemporaryObject(spyComponent, test, { target: RunController, signalName: "runFinished" })
        RunController.setOption("storage", "simulate", "normal")
        verify(RunController.start())
        waitForRendering(screen)
        tryVerify(function() { return findChild(screen, "stopRun").visible }, 5000)
        mouseClick(findChild(screen, "stopRun"))
        finished.wait(30000)
        compare(finished.signalArguments[0][1], "stopped")
        verify(findChild(screen, "queue-storage") !== null)
        const st = findChild(screen, "queue-storage").runState; verify(["cancelled", "done", "not_run"].indexOf(st) >= 0, "state was " + st)
    }
}
