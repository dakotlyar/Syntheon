// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import SynBench
import SynBenchTest

// Results, Detail (every block type from real stub runs), the Battery tab
// (D4, N4), the Stopped "partial" marker (N2) and View log (D16).
TestCase {
    id: test
    name: "ResultsScreen"
    width: 1000
    height: 1000
    visible: true
    when: windowShown

    Component { id: screenComponent; ResultsScreen { width: 1000; height: 1000 } }
    Component { id: spyComponent; SignalSpy {} }

    function initTestCase() {
        tryVerify(function() { return !RunController.probing }, 20000)
        tryVerify(function() { return AppController.authStateName === "ready" }, 10000)
    }

    function runAndWait(ids, options) {
        RunController.clearSelection()
        for (const id of ids) RunController.toggle(id)
        for (const o of (options || [])) RunController.setOption(o[0], o[1], o[2])
        const spy = createTemporaryObject(spyComponent, test, { target: RunController, signalName: "runFinished" })
        verify(RunController.start())
        spy.wait(60000)
        return spy.signalArguments[0]
    }

    function rowFor(screen, id) { return findChild(screen, "result-" + id) }
    function tick() { waitForRendering(test) }

    // Stubs only (Phase 3 N6): the RAM test is real now. The chart and the
    // KPIs come from Battery's page (test_2).
    function test_1_summaryRowsAndDetail() {
        Fixtures.setStubScale(0.02)
        runAndWait(["ai", "storage"])
        const screen = createTemporaryObject(screenComponent, test)
        Results.openRun("")
        tick()
        verify(Results.valid)
        compare(findChild(screen, "overallScore").text, Results.overallText)
        verify(!findChild(screen, "partialMarker").visible)
        verify(!findChild(screen, "statusBadge").visible)
        for (const id of ["ai", "storage"])
            verify(rowFor(screen, id) !== null, id)
        verify(findChild(rowFor(screen, "ai"), "score").text.length > 0)

        // A row opens that test's detail page; the rail's Results stays selected.
        verify(!Results.detailOpen)
        mouseClick(rowFor(screen, "ai"))
        verify(Results.detailOpen)
        tick()
        compare(Results.detailTitle, "AI inference")
        verify(findChild(screen, "detailPage") !== null)
        compare(Results.detailTabs.length, 2)  // the tests of this run, no Battery yet
        verify(findChild(screen, "dtab-ai") !== null)
        // Zeb D2: a chip that takes the keyboard focus draws a ring; the others don't.
        const chip = findChild(screen, "dtab-ai")
        chip.forceActiveFocus(Qt.TabFocusReason)
        verify(chip.visualFocus)
        verify(findChild(chip, "focusRing").visible)
        verify(!findChild(findChild(screen, "dtab-storage"), "focusRing").visible)
        findChild(screen, "dtab-storage").forceActiveFocus(Qt.TabFocusReason)
        verify(!findChild(chip, "focusRing").visible)

        // Another tab: Storage's CrystalDiskMark-style grid.
        mouseClick(findChild(screen, "dtab-storage"))
        tick()
        compare(Results.detailTestId, "storage")
        verify(findChild(screen, "gridMain") !== null)

        mouseClick(findChild(screen, "backToResults"))
        verify(!Results.detailOpen)
    }

    function test_2_batteryTabFromASeparateRun() {
        // A Battery run, then a later standard run: the later run's Detail
        // gets a "Battery" tab labelled as coming from a separate run.
        runAndWait(["battery"], [["battery", "override_preconditions", true], ["battery", "profile", "light"]])
        runAndWait(["ai"])
        const screen = createTemporaryObject(screenComponent, test)
        Results.openRun("")
        tick()
        compare(Results.rows.length, 1)
        const tabs = Results.detailTabs.map(function(t) { return t.id })
        compare(tabs, ["ai", "battery"])
        Results.showDetail("ai")
        tick()
        verify(!findChild(screen, "separateRunNote").visible)
        mouseClick(findChild(screen, "dtab-battery"))
        tick()
        compare(Results.detailTestId, "battery")
        verify(findChild(screen, "separateRunNote").visible)
        verify(findChild(screen, "separateRunNote").text.indexOf("From a separate run") === 0)
        verify(findChild(screen, "separateRunNote").text.indexOf("Light profile") > 0)
        verify(findChild(screen, "chart") !== null)  // the drain curve
        verify(findChild(screen, "chart").points.length > 5)
        verify(findChild(screen, "kpiValue") !== null)
        Results.closeDetail()
        RunController.setOption("battery", "override_preconditions", false)
        RunController.setOption("battery", "profile", "mixed")
    }

    function test_3_viewLogDialog() {
        const screen = createTemporaryObject(screenComponent, test)
        Results.openRun("")
        tick()
        mouseClick(findChild(screen, "viewLog"))
        const copy = findChild(screen, "copyLog")
        tryVerify(function() { return copy !== null && copy.visible }, 5000)
        const view = findChild(screen, "storedLogView")
        tryVerify(function() { return view.count > 0 }, 5000)
        const all = view.count
        const filter = findChild(screen, "logTestFilter")
        filter.chosen("ai")
        verify(view.count > 0 && view.count <= all)
        filter.chosen("ram")  // not in this run's log
        compare(view.count, 0)
        filter.chosen("")
        compare(view.count, all)
        mouseClick(copy)  // copying must not blow up
        keyClick(Qt.Key_Escape)
    }

    // Zeb Z2: a log dialog open on a live run ("View results so far", then
    // "View log") keeps its lines, and its line count, when the next test
    // finishes and Results updates the run underneath it.
    function test_3b_liveLogSurvivesATestFinishing() {
        Fixtures.setStubScale(0.15)
        RunController.clearSelection()
        for (const id of ["ai", "storage"]) RunController.toggle(id)
        const finished = createTemporaryObject(spyComponent, test, { target: RunController, signalName: "runFinished" })
        const updated = createTemporaryObject(spyComponent, test, { target: RunController, signalName: "runUpdated" })
        verify(RunController.start())
        tryVerify(function() { return RunController.log.rowCount() > 2 }, 20000)
        Results.openCurrent()
        const screen = createTemporaryObject(screenComponent, test)
        tick()
        mouseClick(findChild(screen, "viewLog"))
        const view = findChild(screen, "storedLogView")
        const lines = findChild(screen, "lineCount")
        tryVerify(function() { return view.count > 0 }, 5000)
        compare(lines.text, view.count + " lines")
        const shown = view.count
        updated.clear()
        updated.wait(30000)  // the next test finished: Results re-set the live run
        tick()
        compare(view.count, shown)
        compare(lines.text, view.count + " lines")
        finished.wait(60000)  // and the stored copy at the end
        tick()
        compare(view.count, shown)
        compare(lines.text, view.count + " lines")
        keyClick(Qt.Key_Escape)
        Fixtures.setStubScale(0.02)
    }

    // Dave 2026-10-17 (Z4): an N/A test's Detail page is the N/A block with
    // the reason, not its normal layout full of dashes; any test, not just NPU.
    function test_3c_notAvailableDetailShowsTheReason() {
        Fixtures.setStubScale(0.02)
        runAndWait(["ai", "storage"], [["ai", "simulate", "na"]])
        const screen = createTemporaryObject(screenComponent, test)
        Results.openRun("")
        tick()
        Results.showDetail("ai")
        tick()
        const na = findChild(screen, "naStateBlock")
        verify(na !== null && na.visible)
        compare(na.block.title, "AI inference isn't available here")
        verify(na.block.body.indexOf("not available") >= 0, na.block.body)
        compare(Results.detailSubtitle, "Not available on this machine")
        compare(findChild(screen, "kpiValue"), null)  // no empty KPI cards
        // A test that ran keeps its own page.
        Results.showDetail("storage")
        tick()
        compare(findChild(screen, "naStateBlock"), null)
        RunController.setOption("ai", "simulate", "normal")
    }

    function test_4_stoppedRunShowsPartialMarker() {
        Fixtures.setStubScale(0.15)
        RunController.clearSelection()
        RunController.toggle("ai")
        RunController.toggle("storage")
        const spy = createTemporaryObject(spyComponent, test, { target: RunController, signalName: "runFinished" })
        verify(RunController.start())
        // Stop once the first test has a score.
        // (StateRole is Qt::UserRole + 4)
        tryVerify(function() { return RunController.queue.rowCount() === 2 && RunController.queue.data(RunController.queue.index(0, 0), 0x0100 + 4) === "done" }, 30000)
        RunController.stop()
        spy.wait(30000)
        compare(spy.signalArguments[0][1], "stopped")
        const screen = createTemporaryObject(screenComponent, test)
        Results.openRun("")
        tick()
        compare(Results.statusLabel, "Stopped")
        verify(findChild(screen, "statusBadge").visible)
        compare(findChild(screen, "statusBadge").text, "Stopped")
        verify(findChild(screen, "partialMarker").visible)  // N2
        verify(findChild(screen, "overallNote").text.indexOf("Partial") === 0)
        // Storage runs first (catalog order); AI was either mid-run (Stopped)
        // or never started (Not run).
        verify(["Stopped", "Not run"].indexOf(findChild(rowFor(screen, "ai"), "score").text) >= 0)
        Fixtures.setStubScale(0.02)
    }

    function test_5_noResultsAfterClearHistory() {
        StorageController.clearHistory()
        const screen = createTemporaryObject(screenComponent, test)
        verify(!Results.valid)
        verify(findChild(screen, "emptyState").visible)
        verify(!findChild(screen, "viewLog").visible)
    }
}
