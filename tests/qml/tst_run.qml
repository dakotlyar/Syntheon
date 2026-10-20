// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import SynBench
import SynBenchTest

// The Run screen on the real controllers (stubs, probes, selection rules).
TestCase {
    id: test
    name: "RunScreen"
    width: 1000
    height: 900
    visible: true
    when: windowShown

    Component { id: screenComponent; RunScreen { width: 1000; height: 900 } }

    function init() {
        RunController.clearSelection()
        tryVerify(function() { return !RunController.probing }, 20000)
        tryVerify(function() { return AppController.authStateName === "ready" }, 10000)
    }

    function row(screen, id) { return findChild(screen, "pick-" + id) }

    function test_pickerListsEverySeedTest() {
        const screen = createTemporaryObject(screenComponent, test)
        for (const id of ["cpu", "ram", "storage", "gpu", "ai", "npu", "battery"])
            verify(row(screen, id) !== null, id)
        compare(RunController.catalog.rowCount(), 7)
    }

    function test_tickingShowsOptionTabsAndEstimate() {
        const screen = createTemporaryObject(screenComponent, test)
        compare(findChild(screen, "selectedCount").text, "0 tests selected")
        RunController.toggle("cpu")
        RunController.toggle("ram")
        waitForRendering(screen)  // layouts settle on the next frame
        compare(findChild(screen, "selectedCount").text, "2 tests selected")
        verify(findChild(screen, "estimate").visible)
        verify(findChild(screen, "estimate").text.indexOf("estimated") >= 0)
        verify(findChild(screen, "tab-cpu") !== null)
        verify(findChild(screen, "tab-ram") !== null)
        compare(RunController.activeTab, "ram")  // the newest tick
        tryVerify(function() { return findChild(screen, "tab-cpu").width > 0 && findChild(screen, "tab-cpu").mapToItem(screen, 0, 0).y > 0 })
        mouseClick(findChild(screen, "tab-cpu"))
        compare(RunController.activeTab, "cpu")
        verify(RunController.options !== null)
        compare(findChild(screen, "runButton").text, "Run selected tests")
        verify(findChild(screen, "runButton").enabled)
    }

    function test_selectAllSkipsBatteryAndUnavailable() {
        const screen = createTemporaryObject(screenComponent, test)
        mouseClick(findChild(screen, "selectAll"))
        compare(RunController.selectedCount, 5)  // not Battery, not the N/A NPU
        compare(findChild(screen, "runButton").text, "Run selected tests")
        mouseClick(findChild(screen, "clearSelection"))
        compare(RunController.selectedCount, 0)
        verify(!findChild(screen, "runButton").enabled)
    }

    function test_batteryRunsAlone() {
        const screen = createTemporaryObject(screenComponent, test)
        RunController.toggle("cpu")
        RunController.toggle("ram")
        mouseClick(row(screen, "battery"))
        compare(RunController.selectedCount, 1)
        verify(RunController.runsAloneSelected)
        compare(findChild(screen, "runButton").text, "Start battery run")
        verify(findChild(screen, "estimate").text.indexOf("battery is empty") >= 0)
        mouseClick(row(screen, "gpu"))  // ticking anything else clears Battery
        compare(RunController.selectedCount, 1)
        verify(!RunController.runsAloneSelected)
    }

    function test_unavailableTestCantBeTicked() {
        const screen = createTemporaryObject(screenComponent, test)
        mouseClick(row(screen, "npu"))
        compare(RunController.selectedCount, 0)
    }

    // The power state is faked (the input only, via Fixtures.setPower), so
    // this is the same on any host; the preconditions come from it as usual.
    function test_batteryPreconditionsBlockStartUntilOverridden_data() {
        return [
            { tag: "on AC", present: true, ac: true, charge: 100, notMet: ["unplugged"] },
            { tag: "on battery, full", present: true, ac: false, charge: 100, notMet: [] },
            { tag: "on battery, low", present: true, ac: false, charge: 40, notMet: ["charged"] },
            { tag: "no battery", present: false, ac: false, charge: -1, notMet: ["battery", "unplugged", "charged"] },
        ]
    }

    function test_batteryPreconditionsBlockStartUntilOverridden(data) {
        Fixtures.setPower(data.present, data.ac, data.charge)
        const screen = createTemporaryObject(screenComponent, test)
        mouseClick(row(screen, "battery"))
        const notMet = function() {
            return RunController.preconditions.filter(p => p.state === "notmet").map(p => p.id).sort()
        }
        tryVerify(function() { return RunController.preconditions.length > 0 }, 5000)
        tryVerify(function() { return notMet().length === data.notMet.length }, 5000)
        compare(notMet(), data.notMet.slice().sort())
        const button = findChild(screen, "runButton")
        if (data.notMet.length === 0) {
            // Nothing NotMet: Start is open without the override.
            tryVerify(function() { return RunController.canStart }, 5000)
            verify(button.enabled)
            verify(findChild(screen, "problem") === null)
        } else {
            // Something NotMet: Start stays blocked...
            verify(!RunController.canStart)
            verify(!button.enabled)
            verify(RunController.problems.length > 0)
            verify(findChild(screen, "problem") !== null)
            // ...until the Phase 1 override clears it.
            RunController.setOption("battery", "override_preconditions", true)
            tryVerify(function() { return RunController.canStart }, 5000)
            verify(button.enabled)
            compare(notMet(), [])
            RunController.setOption("battery", "override_preconditions", false)
        }
    }

    function cleanup() {
        Fixtures.clearPower()
    }

    // A stub's option (Phase 3 N6: the RAM stub's duration option is gone).
    function test_optionChangesTheEstimate() {
        const screen = createTemporaryObject(screenComponent, test)
        RunController.toggle("storage")
        const before = RunController.estimateText
        RunController.setOption("storage", "file_size", "4g")
        verify(RunController.estimateText !== before)
        RunController.setOption("storage", "file_size", "1g")
        compare(RunController.estimateText, before)
    }

    function test_nothingSelectedShowsAHint() {
        const screen = createTemporaryObject(screenComponent, test)
        verify(RunController.tabs.length === 0)
        verify(RunController.options === null)
    }
}
