// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import Syntheon

// D1: the administrator gate offers Retry and Quit, nothing else.
TestCase {
    id: test
    name: "AuthDialog"
    width: 800
    height: 600
    visible: true
    when: windowShown

    Component { id: spyComponent; SignalSpy {} }
    Component {
        id: dialogComponent
        AuthDialog { parent: test }
    }

    function test_failedOffersRetryAndQuit() {
        const d = createTemporaryObject(dialogComponent, test, { message: "The administrator prompt was cancelled." })
        d.open()
        tryVerify(function() { return d.opened })
        const retry = findChild(d.footer, "authRetry")
        const quit = findChild(d.footer, "authQuit")
        verify(retry.visible)
        verify(quit.visible)
        const retrySpy = createTemporaryObject(spyComponent, test, { target: d, signalName: "retryClicked" })
        const quitSpy = createTemporaryObject(spyComponent, test, { target: d, signalName: "quitClicked" })
        mouseClick(retry)
        compare(retrySpy.count, 1)
        compare(quitSpy.count, 0)
        mouseClick(quit)
        compare(quitSpy.count, 1)
        d.close()
    }

    function test_authorisingOnlyOffersQuit() {
        const d = createTemporaryObject(dialogComponent, test, { authorising: true })
        d.open()
        tryVerify(function() { return d.opened })
        verify(!findChild(d.footer, "authRetry").visible)
        verify(findChild(d.footer, "authQuit").visible)  // a way out while the prompt is open
        d.close()
    }

    function test_cantBeDismissed() {
        const d = createTemporaryObject(dialogComponent, test, {})
        d.open()
        tryVerify(function() { return d.opened })
        keyClick(Qt.Key_Escape)
        verify(d.opened)  // blocking: no limited mode
        verify(d.modal)
        d.close()
    }
}
