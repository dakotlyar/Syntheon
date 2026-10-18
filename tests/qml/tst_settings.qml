// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import SynBench
import SynBenchTest

// The Settings screen on real controllers: layout and theme apply right away
// and persist; Clear history asks first and then really clears.
TestCase {
    id: test
    name: "SettingsScreen"
    width: 900
    height: 700
    visible: true
    when: windowShown

    Binding { target: Theme; property: "mode"; value: Settings.theme }

    Component { id: screenComponent; SettingsScreen { width: 900; height: 700 } }
    Component { id: spyComponent; SignalSpy {} }

    function test_layoutAndTheme() {
        const screen = createTemporaryObject(screenComponent, test)
        const layout = findChild(screen, "layoutSegmented")
        const theme = findChild(screen, "themeSegmented")
        compare(Settings.layout, "rail")  // the default
        compare(layout.current, "rail")

        // Click "Top tabs": the third item of the theme control is Dark, the second of layout is Top tabs.
        layout.activated("tabs")
        compare(Settings.layout, "tabs")
        compare(layout.current, "tabs")
        theme.activated("dark")
        compare(Settings.theme, "dark")
        verify(Theme.dark)  // live
        theme.activated("light")
        verify(!Theme.dark)
        Settings.layout = "rail"
        Settings.theme = "system"
    }

    function test_segmentedClickActivates() {
        const screen = createTemporaryObject(screenComponent, test)
        const layout = findChild(screen, "layoutSegmented")
        const spy = createTemporaryObject(spyComponent, test, { target: layout, signalName: "activated" })
        mouseClick(layout, layout.width - 5, layout.height / 2)  // the last segment
        compare(spy.count, 1)
        compare(spy.signalArguments[0][0], "tabs")
        Settings.layout = "rail"
    }
    function test_showsTheHistoryLocation() {
        const screen = createTemporaryObject(screenComponent, test)
        verify(findChild(screen, "dataPath").text.endsWith("synbench.db"))
        verify(findChild(screen, "historyHint").text.indexOf("saved run") >= 0)
    }

    function test_clearHistoryAsksFirst() {
        if (StorageController.runCount === 0) {
            // Earlier tests may have cleared the history: make a run to clear.
            Fixtures.setStubScale(0.02)
            RunController.clearSelection()
            RunController.toggle("gpu")
            const spy = createTemporaryObject(spyComponent, test, { target: RunController, signalName: "runFinished" })
            verify(RunController.start())
            spy.wait(60000)
        }
        const screen = createTemporaryObject(screenComponent, test)
        const count = StorageController.runCount
        verify(count > 0)
        verify(!findChild(screen, "clearConfirm").visible)
        mouseClick(findChild(screen, "clearHistory"))
        verify(findChild(screen, "clearConfirm").visible)
        compare(StorageController.runCount, count)  // nothing deleted yet
        // Cancel
        screen.confirmingClear = false
        verify(!findChild(screen, "clearConfirm").visible)
        compare(StorageController.runCount, count)

        mouseClick(findChild(screen, "clearHistory"))
        mouseClick(findChild(screen, "clearYes"))
        compare(StorageController.runCount, 0)
        verify(!findChild(screen, "clearConfirm").visible)
        verify(!findChild(screen, "clearHistory").enabled)  // nothing left to clear
    }
}
