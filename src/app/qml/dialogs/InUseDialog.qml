// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// The history is locked, but no running SynBench answered. Either a copy is
// open and unresponsive, or an earlier run ended abnormally and left its lock.
Dialog {
    id: dialog

    property string error
    signal takeOver()
    signal quitClicked()

    modal: true
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 48 : 500, 500)
    padding: 18
    title: qsTr("SynBench may already be open")
    background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: 8 }
    header: SbText { text: dialog.title; kind: "title"; leftPadding: 18; topPadding: 16; wrapMode: Text.WordWrap }

    contentItem: ColumnLayout {
        spacing: 10
        SbText {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("SynBench may already be open, or a previous run ended abnormally. If another SynBench window is open, quit and use that one. Otherwise take over.")
        }
        SbText {
            objectName: "inUseError"
            Layout.fillWidth: true
            visible: dialog.error.length > 0
            wrapMode: Text.WordWrap
            text: dialog.error
            color: Theme.bad
            kind: "small"
        }
    }
    footer: DialogButtonBox {
        background: Item {}
        SbButton { objectName: "takeOver"; text: qsTr("Take over"); onClicked: dialog.takeOver() }
        SbButton { objectName: "quit"; primary: true; text: qsTr("Quit"); onClicked: dialog.quitClicked() }
    }
}
