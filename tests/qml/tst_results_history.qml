// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import Syntheon
import SyntheonTest

// History (tick, Open, Compare) and Compare, driven by real stub runs.
TestCase {
    id: test
    name: "HistoryScreen"
    width: 1000
    height: 900
    visible: true
    when: windowShown

    Component { id: historyComponent; HistoryScreen { width: 1000; height: 900 } }
    Component { id: compareComponent; CompareScreen { width: 1000; height: 900 } }
    Component { id: spyComponent; SignalSpy {} }

    function initTestCase() {
        tryVerify(function() { return !RunController.probing }, 20000)
        tryVerify(function() { return AppController.authStateName === "ready" }, 10000)
    }

    function oneRun(id) {
        Fixtures.setStubScale(0.02)
        RunController.clearSelection()
        RunController.toggle(id)
        const spy = createTemporaryObject(spyComponent, test, { target: RunController, signalName: "runFinished" })
        verify(RunController.start())
        spy.wait(60000)
    }
    function tick() { waitForRendering(test) }

    function test_1_emptyHistoryAndCompareGuide() {
        StorageController.clearHistory()
        const h = createTemporaryObject(historyComponent, test)
        const c = createTemporaryObject(compareComponent, test)
        tick()
        compare(History.totalCount, 0)
        verify(findChild(h, "emptyState").visible)
        verify(findChild(c, "emptyState").visible)
        verify(!findChild(h, "compareButton").enabled)
    }

    function test_2_tickTwoThenCompare() {
        oneRun("ai")
        oneRun("gpu")  // the Storage stub leaves in Phase 4b; the GPU test's worker is a fake
        const h = createTemporaryObject(historyComponent, test)
        tick()
        compare(History.totalCount, 2)
        verify(!findChild(h, "emptyState").visible)
        const button = findChild(h, "compareButton")
        verify(!button.enabled)
        compare(button.text, "Pick 2 or more")
        const checks = []
        for (let i = 0; i < 2; ++i) {
            const row = findChild(h, "run-" + History.data(History.index(i, 0), 257))
            verify(row !== null)
            mouseClick(findChild(row, "check"))
        }
        compare(History.checkedCount, 2)
        verify(button.enabled)
        compare(button.text, "Compare 2 runs")
        AppController.section = "history"

        mouseClick(button)
        tryCompare(AppController, "section", "compare")
        verify(Compare.valid)
        compare(Compare.runs.length, 2)
    }

    function allNamed(item, name) {
        let out = []
        if (item.objectName === name && item.visible) out.push(item)
        for (let i = 0; i < item.children.length; ++i) out = out.concat(allNamed(item.children[i], name))
        return out
    }

    // Zeb re-check: the baseline column's header is as tall as the others and its
    // date and time sit at the same height.
    function test_3b_runHeadersLineUp() {
        const c = createTemporaryObject(compareComponent, test)
        tick()
        const days = allNamed(c, "runDay")
        const times = allNamed(c, "runTime")
        verify(days.length >= 2, "run headers: " + days.length)
        const y0 = days[0].mapToItem(c, 0, 0).y
        const t0 = times[0].mapToItem(c, 0, 0).y
        for (let i = 1; i < days.length; ++i) {
            compare(days[i].mapToItem(c, 0, 0).y, y0)
            compare(times[i].mapToItem(c, 0, 0).y, t0)
        }
    }

    function test_3_compareScreenShowsColumnsAndSwitchesBaseline() {
        const c = createTemporaryObject(compareComponent, test)
        tick()
        verify(!findChild(c, "emptyState").visible)
        verify(findChild(c, "scoreTable").visible)
        verify(findChild(c, "rawTable").visible)
        const newest = Compare.runs[0].uuid
        compare(Compare.baselineUuid, newest)  // D5
        const older = Compare.runs[1].uuid
        Compare.setBaseline(older)
        compare(Compare.baselineUuid, older)
        tick()
        // Zeb: a run without a test's score leaves its bar's slot empty, and
        // every other bar stays in its run's slot. One run has only AI, the
        // other only GPU: in each test row one slot is empty.
        const slots = allSlots(c)
        verify(slots.length >= 2, "bar rows: " + slots.length)
        let empty = 0
        for (const row of slots) {
            compare(row.length, Compare.runs.length)
            for (let i = 0; i < row.length; ++i) {
                // slot i of every row at the same offset: i x (6 + 3 spacing)
                compare(row[i].y, i * 9)
                const bar = findChild(row[i], "bar")
                if (!bar.visible) ++empty
                else verify(bar.width >= 2)
            }
        }
        verify(empty >= 2, "empty slots: " + empty)
    }

    // The bar slots of each relative-bars cell, in run order.
    function allSlots(item) {
        let out = []
        const mine = []
        for (let i = 0; i < item.children.length; ++i)
            if (item.children[i].objectName === "barSlot") mine.push(item.children[i])
        if (mine.length > 0 && item.visible) out.push(mine.sort((a, b) => a.index - b.index))
        for (let i = 0; i < item.children.length; ++i) out = out.concat(allSlots(item.children[i]))
        return out
    }

    function test_4_openShowsResults() {
        const h = createTemporaryObject(historyComponent, test)
        tick()
        const uuid = History.data(History.index(1, 0), 257)
        mouseClick(findChild(findChild(h, "run-" + uuid), "open"))
        compare(AppController.section, "results")
        compare(Results.uuid, uuid)
    }

    // AP-5 #2: History > Open must stay on the run that was opened, even though
    // this session also has a (different, newer) run of its own.
    function test_5_openedRunIsNotReplacedByTheSessionsRun() {
        const h = createTemporaryObject(historyComponent, test)
        const r = createTemporaryObject(resultsComponent, test, { visible: false })
        tick()
        verify(RunController.hasRun)
        const older = History.data(History.index(1, 0), 257)
        verify(older !== RunController.runUuid)
        mouseClick(findChild(findChild(h, "run-" + older), "open"))
        compare(AppController.section, "results")
        r.visible = true  // the Results page is shown after the click
        tick()
        compare(Results.uuid, older)
    }

    Component { id: resultsComponent; ResultsScreen { width: 1000; height: 900 } }
}
