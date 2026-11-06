// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import Syntheon

// §7/§10: switching rail <-> top tabs swaps only the nav, so every screen keeps
// its state; a narrow window falls back to the top strip by itself.
TestCase {
    id: test
    name: "AppShell"
    width: 900
    height: 600
    visible: true
    when: windowShown

    Component {
        id: shellComponent
        AppShell {
            width: 900
            height: 600
            // One child per section, in order; each remembers something the user typed.
            Item { objectName: "page-run"; property string typed: "" }
            Item { objectName: "page-progress"; property string typed: "" }
            Item { objectName: "page-results"; property string typed: "" }
            Item { objectName: "page-history"; property string typed: "" }
            Item { objectName: "page-compare"; property string typed: "" }
            Item { objectName: "page-system"; property string typed: "" }
            Item { objectName: "page-settings"; property string typed: "" }
        }
    }

    function nav(shell) { return findChild(shell, "navLoader").item }

    function test_railIsTheDefaultAndNavigates() {
        const shell = createTemporaryObject(shellComponent, test)
        verify(shell.useRail)
        verify(nav(shell) !== null)
        compare(nav(shell).objectName, "")  // a RailNav (not the strip)
        const spy = createTemporaryObject(spyComponent, test, { target: shell, signalName: "navigate" })
        mouseClick(findChild(nav(shell), "nav-history"))
        compare(spy.count, 1)
        compare(spy.signalArguments[0][0], "history")
    }

    Component { id: spyComponent; SignalSpy {} }

    function test_sectionSelectsThePage() {
        const shell = createTemporaryObject(shellComponent, test)
        compare(shell.currentIndex, 0)
        shell.section = "history"
        compare(shell.currentIndex, 3)
        shell.section = "settings"
        compare(shell.currentIndex, 6)  // the spacer isn't a page
        shell.section = "results"
        compare(shell.currentIndex, 2)
    }

    function test_layoutSwitchKeepsScreenState() {
        const shell = createTemporaryObject(shellComponent, test)
        const run = findChild(shell, "page-run")
        const history = findChild(shell, "page-history")
        run.typed = "half-typed option"
        history.typed = "scrolled"
        shell.section = "history"
        const railNav = nav(shell)

        shell.layout = "tabs"
        verify(!shell.useRail)
        verify(nav(shell) !== railNav)  // the nav was swapped...
        compare(findChild(shell, "page-run"), run)  // ...the screens weren't
        compare(run.typed, "half-typed option")
        compare(history.typed, "scrolled")
        compare(shell.currentIndex, 3)
        verify(findChild(nav(shell), "nav-history") !== null)

        shell.layout = "rail"
        compare(run.typed, "half-typed option")
        compare(findChild(shell, "page-history"), history)
    }

    function test_narrowWindowUsesTopStrip() {
        const shell = createTemporaryObject(shellComponent, test)
        shell.width = 700
        verify(shell.narrow)
        verify(!shell.useRail)
        shell.width = 900
        verify(shell.useRail)
        // ...but an explicit top-tabs layout stays tabs on a wide window.
        shell.layout = "tabs"
        verify(!shell.useRail)
    }

    // Zeb: at the 760 px minimum the top tabs all fit; Settings isn't cut off.
    function test_topTabsFitAt760() {
        const shell = createTemporaryObject(shellComponent, test, { width: 760 })
        shell.layout = "tabs"
        waitForRendering(shell)
        const strip = nav(shell)
        const settings = findChild(strip, "nav-settings")
        verify(settings !== null)
        const right = settings.mapToItem(strip, settings.width, 0).x
        verify(right <= strip.width, "Settings ends at " + right + " of " + strip.width)
    }

    function test_navReflectsTheCurrentSection() {
        const shell = createTemporaryObject(shellComponent, test)
        shell.section = "compare"
        verify(findChild(nav(shell), "nav-compare").Accessible.selected)
        verify(!findChild(nav(shell), "nav-run").Accessible.selected)
        verify(findChild(nav(shell), "nav-settings") !== null)  // after the spacer
    }
}
