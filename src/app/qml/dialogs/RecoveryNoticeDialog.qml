// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// One-time notice after a run was cut short last time (design 3): "The run
// from 14:02 was interrupted" or "Battery run recovered: 6 h 42 m".
SbDialog {
    id: dialog

    property string message
    signal dismissed()

    width: Math.min(parent ? parent.width - 48 : 460, 460)
    title: qsTr("A run was cut short")

    contentItem: ColumnLayout {
        spacing: 10
        SbText { objectName: "noticeText"; Layout.fillWidth: true; wrapMode: Text.WordWrap; text: dialog.message }
        SbText {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            kind: "small"
            text: qsTr("What was finished before it stopped is in History.")
        }
    }
    footer: DialogButtonBox {
        background: Item {}
        SbButton { objectName: "dismiss"; primary: true; text: qsTr("OK"); onClicked: dialog.dismissed() }
    }
}
