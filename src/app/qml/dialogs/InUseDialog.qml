// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Syntheon

// The history is locked, but no running Syntheon answered. Either a copy is
// open and unresponsive, or an earlier run ended abnormally and left its lock.
SbDialog {
    id: dialog

    property string error
    signal takeOver()
    signal quitClicked()

    width: Math.min(parent ? parent.width - 48 : 500, 500)
    title: qsTr("Syntheon may already be open")

    contentItem: ColumnLayout {
        spacing: 10
        SbText {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Syntheon may already be open, or a previous run ended abnormally. If another Syntheon window is open, quit and use that one. Otherwise take over.")
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
