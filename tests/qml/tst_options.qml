// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import Syntheon
import SyntheonTest

// §10: every option editor kind renders and writes its value (P7).
TestCase {
    id: test
    name: "OptionEditors"
    width: 700
    height: 900
    visible: true
    when: windowShown

    Component {
        id: tabComponent
        OptionTab { width: 640 }
    }

    function makeTab(preconditions) {
        const model = Fixtures.allKindsModel()
        const tab = createTemporaryObject(tabComponent, test, { optionsModel: model, preconditions: preconditions || [] })
        verify(tab)
        return { tab: tab, model: model }
    }

    function editor(tab, key) { return findChild(tab, "option-" + key) }

    function test_everyKindRenders() {
        const t = makeTab()
        for (const key of ["flag", "multi", "seg", "drop", "dyn", "num", "path", "info"])
            verify(editor(t.tab, key) !== null, key)
        verify(findChild(editor(t.tab, "flag"), "bool").visible)
        verify(findChild(editor(t.tab, "multi"), "multi-a").visible)
        verify(findChild(editor(t.tab, "seg"), "segmented").visible)
        verify(findChild(editor(t.tab, "drop"), "combo").visible)
        verify(findChild(editor(t.tab, "dyn"), "refresh").visible)  // only DynamicSelect has Refresh
        verify(!findChild(editor(t.tab, "drop"), "refresh").visible)
        verify(findChild(editor(t.tab, "num"), "number").visible)
        verify(findChild(editor(t.tab, "path"), "path").visible)
        verify(findChild(editor(t.tab, "info"), "info").visible)
    }

    // Zeb #1: a long, non-neutral pill wraps inside the row, never over the label.
    function test_longInfoPillStaysInsideItsRow() {
        const t = makeTab()
        waitForRendering(t.tab)
        const row = editor(t.tab, "longinfo")
        const label = findChild(row, "infoLabel")
        const pill = findChild(row, "infoPill")
        verify(pill.visible)
        const labelRight = label.mapToItem(row, label.width, 0).x
        const pillLeft = pill.mapToItem(row, 0, 0).x
        const pillRight = pill.mapToItem(row, pill.width, 0).x
        verify(pillLeft >= labelRight, "pill starts at " + pillLeft + ", label ends at " + labelRight)
        verify(pillRight <= row.width + 0.5, "pill ends at " + pillRight + " of " + row.width)
        const text = findChild(pill, "pillText")
        verify(text.lineCount >= 1)
        verify(text.contentWidth <= pill.width)  // wrapped, not clipped wide
    }

    function test_boolWrites() {
        const t = makeTab()
        compare(t.model.value("flag"), true)
        mouseClick(findChild(editor(t.tab, "flag"), "bool"))
        compare(t.model.value("flag"), false)
    }

    function test_multiSelectWritesAndDisablesWithReason() {
        const t = makeTab()
        const a = findChild(editor(t.tab, "multi"), "multi-a")
        const b = findChild(editor(t.tab, "multi"), "multi-b")
        const c = findChild(editor(t.tab, "multi"), "multi-c")
        verify(a.checked)
        verify(!b.checked)
        verify(!c.enabled)
        verify(c.text.indexOf("no Intel device") >= 0)  // the reason is shown...
        compare(c.text.split("no Intel device").length - 1, 1)  // ...once
        const d = findChild(editor(t.tab, "multi"), "multi-d")
        compare(d.text, "Delta (no NVIDIA GPU)")  // already in the label: not appended again
        mouseClick(b)
        compare(t.model.value("multi"), ["a", "b"])
        mouseClick(a)
        compare(t.model.value("multi"), ["b"])
        mouseClick(c)  // disabled: nothing happens
        compare(t.model.value("multi"), ["b"])
    }

    function test_segmentedWrites() {
        const t = makeTab()
        const seg = findChild(editor(t.tab, "seg"), "segmented")
        compare(seg.current, "m")
        seg.activated("l")
        compare(t.model.value("seg"), "l")
        compare(seg.current, "l")
    }

    function test_dropdownAndDynamicWrite() {
        const t = makeTab()
        const drop = findChild(editor(t.tab, "drop"), "combo")
        compare(drop.current, "g")
        drop.chosen("s")
        compare(t.model.value("drop"), "s")
        const dyn = findChild(editor(t.tab, "dyn"), "combo")
        compare(dyn.items.length, 2)
        verify(dyn.items[0].detail.indexOf("NVMe") >= 0)  // path · type · free · fs
        dyn.chosen("/mnt")
        compare(t.model.value("dyn"), "/mnt")
    }

    function test_dynamicRefreshButtonEmitsRequest() {
        const t = makeTab()
        const spy = createTemporaryObject(spyComponent, test, { target: t.tab, signalName: "refreshRequested" })
        mouseClick(findChild(editor(t.tab, "dyn"), "refresh"))
        compare(spy.count, 1)
    }
    Component { id: spyComponent; SignalSpy {} }

    function test_numberWritesWithinRange() {
        const t = makeTab()
        const num = findChild(editor(t.tab, "num"), "number")
        compare(t.model.value("num"), 10)
        num.edited(25)
        compare(t.model.value("num"), 25)
        num.edited(500)  // clamped to the schema's maximum
        compare(t.model.value("num"), 60)
        num.edited(1)
        compare(t.model.value("num"), 5)
    }

    function test_pathWrites() {
        const t = makeTab()
        const path = findChild(editor(t.tab, "path"), "path")
        path.text = "/srv/bench"
        path.editingFinished()
        compare(t.model.value("path"), "/srv/bench")
    }

    function test_infoIsReadOnly() {
        const t = makeTab()
        t.model.setValue("info", "x")  // ignored: Info has no value
        compare(t.model.value("info"), "")
    }

    function test_preconditionsList() {
        const t = makeTab([
            { id: "unplugged", label: "Running on battery", state: "notmet", detail: "On AC: unplug" },
            { id: "brightness", label: "Screen brightness locked", state: "apply", detail: "Will lock at start" }])
        verify(findChild(t.tab, "preconditions").visible)
        verify(findChild(t.tab, "precondition-unplugged") !== null)
        verify(findChild(t.tab, "precondition-brightness") !== null)
        const none = makeTab([])
        verify(!findChild(none.tab, "preconditions").visible)
    }

    // AP-5: a precondition label now carries a long guided-setup sentence + URL
    // (npu_test.cpp); it must wrap instead of overflowing the pane, and keep the
    // status pill inside the row.
    function test_longPreconditionLabelWraps() {
        const url = "https://docs.openvino.ai/latest/get-started.html"
        const t = makeTab([{ id: "runtime", state: "notmet", detail: "Not installed",
                             label: "An Intel NPU was found, but its runtime isn't installed — install it: " + url }])
        waitForRendering(t.tab)
        const row = findChild(t.tab, "precondition-runtime")
        verify(row !== null)
        const label = findChild(row, "preconditionLabel")
        const pill = findChild(row, "preconditionPill")
        verify(label.visible)
        verify(pill.visible)
        verify(label.lineCount > 1, "label wrapped to " + label.lineCount + " line(s)")
        const pillRight = pill.mapToItem(row, pill.width, 0).x
        verify(pillRight <= row.width + 0.5, "pill ends at " + pillRight + " of " + row.width)
    }

    function test_nothingYetShowsAMessage() {
        const tab = createTemporaryObject(tabComponent, test, { optionsModel: null })
        verify(tab.children.length > 0)
    }
}
