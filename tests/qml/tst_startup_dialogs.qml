// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import SynBench

// The first-run dialog (hardware check + data location) and the one-time
// recovery notice.
TestCase {
    id: test
    name: "StartupDialogs"
    width: 900
    height: 700
    visible: true
    when: windowShown

    Component { id: firstRun; FirstRunDialog { parent: test } }
    Component { id: notice; RecoveryNoticeDialog { parent: test } }
    Component { id: inUse; InUseDialog { parent: test } }
    Component { id: spyComponent; SignalSpy {} }

    function initTestCase() {
        tryVerify(function() { return !RunController.probing }, 20000)
    }

    function test_firstRunListsEveryTestAndTheDataLocation() {
        const d = createTemporaryObject(firstRun, test)
        d.open()
        waitForRendering(test)
        for (const id of ["cpu", "ram", "storage", "gpu", "ai", "npu", "battery"])
            verify(findChild(d.contentItem, "check-" + id) !== null, id)
        compare(findChild(d.contentItem, "dataLocation").text, StorageController.dataLocation)
        const spy = createTemporaryObject(spyComponent, test, { target: d, signalName: "finished" })
        mouseClick(findChild(d.footer, "getStarted"))
        compare(spy.count, 1)
    }

    function test_recoveryNoticeShowsItsMessageAndDismisses() {
        const d = createTemporaryObject(notice, test, { message: "Battery run recovered: 6 h 42 m" })
        d.open()
        waitForRendering(test)
        compare(findChild(d.contentItem, "noticeText").text, "Battery run recovered: 6 h 42 m")
        const spy = createTemporaryObject(spyComponent, test, { target: d, signalName: "dismissed" })
        mouseClick(findChild(d.footer, "dismiss"))
        compare(spy.count, 1)
    }

    function test_inUseDialogOffersTakeOverAndQuit() {
        const d = createTemporaryObject(inUse, test, { error: "" })
        d.open()
        waitForRendering(test)
        verify(!findChild(d.contentItem, "inUseError").visible)
        const take = createTemporaryObject(spyComponent, test, { target: d, signalName: "takeOver" })
        const quit = createTemporaryObject(spyComponent, test, { target: d, signalName: "quitClicked" })
        mouseClick(findChild(d.footer, "takeOver"))
        compare(take.count, 1)
        d.error = "No luck"
        verify(findChild(d.contentItem, "inUseError").visible)
        mouseClick(findChild(d.footer, "quit"))
        compare(quit.count, 1)
    }
}
