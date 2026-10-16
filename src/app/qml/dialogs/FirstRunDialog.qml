// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// Shown once, after the storage and administrator gates: what SynBench found
// (one line per test, from its probe) and where your history is kept.
SbDialog {
    id: dialog

    signal finished()

    width: Math.min(parent ? parent.width - 48 : 560, 560)

    title: qsTr("Welcome to SynBench")
    subtitle: qsTr("Here is what this machine can run.")

    contentItem: ColumnLayout {
        spacing: 12

        Box {
            objectName: "hardwareCheck"
            Layout.fillWidth: true
            implicitHeight: checkCol.implicitHeight
            ColumnLayout {
                id: checkCol
                width: parent.width
                spacing: 0
                SbText {
                    visible: !Startup.hardwareReady
                    text: qsTr("Checking your hardware…")
                    padding: 12
                }
                Repeater {
                    model: Startup.hardwareCheck
                    delegate: Item {
                        required property var modelData
                        required property int index
                        objectName: "check-" + modelData.id
                        Layout.fillWidth: true
                        implicitHeight: Math.max(40, detail.implicitHeight + 18)
                        Rectangle { visible: index > 0; width: parent.width; height: 1; color: Theme.line }
                        Swatch { x: 12; anchors.verticalCenter: parent.verticalCenter; implicitHeight: 22; color: Theme.testColor(modelData.colorToken) }
                        SbText { x: 30; width: 110; anchors.verticalCenter: parent.verticalCenter; text: modelData.name; font.weight: Font.DemiBold }
                        SbText {
                            id: detail
                            x: 146; width: parent.width - 146 - 12
                            anchors.verticalCenter: parent.verticalCenter
                            wrapMode: Text.WordWrap
                            kind: "small"
                            text: modelData.detail.length > 0 ? modelData.detail : (modelData.state === "na" ? qsTr("Not available") : qsTr("Ready"))
                            color: modelData.state === "na" ? Theme.na : Theme.ink
                        }
                    }
                }
            }
        }

        SbText { text: qsTr("Where your runs are kept"); font.weight: Font.DemiBold }
        SbText {
            objectName: "dataLocation"
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            text: StorageController.dataLocation
            kind: "data"
            font.pixelSize: 12
        }
        SbText {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            kind: "small"
            text: qsTr("Every run is saved there so you can compare later. You can move it in Settings.")
        }
    }

    footer: DialogButtonBox {
        background: Item {}
        SbButton { objectName: "getStarted"; primary: true; text: qsTr("Get started"); onClicked: dialog.finished() }
    }
}
