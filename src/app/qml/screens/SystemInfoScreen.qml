// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Syntheon

// What Syntheon found on this machine; the same snapshot is saved with every
// run. No serial numbers, UUIDs or MAC addresses are ever collected. The AI
// runtimes panel arrives with the AI test in Phase 6.
ScreenFrame {
    id: root

    title: qsTr("System info")
    subtitle: qsTr("Captured automatically with every run")

    GridLayout {
        Layout.fillWidth: true
        columns: root.width > 900 ? 2 : 1
        columnSpacing: 16
        rowSpacing: 16

        Box {
            objectName: "systemTable"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            implicitHeight: tableCol.implicitHeight

            SbText {
                objectName: "loading"
                visible: !AppController.systemInfoReady
                text: qsTr("Reading system information…")
                padding: 16
            }
            ColumnLayout {
                id: tableCol
                width: parent.width
                spacing: 0
                visible: AppController.systemInfoReady
                Repeater {
                    model: AppController.systemRows
                    delegate: Item {
                        id: sysRow
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        implicitHeight: Math.max(44, valueText.implicitHeight + 24)
                        Rectangle {
                            visible: sysRow.index > 0
                            width: parent.width; height: 1; color: Theme.line
                        }
                        SbText {
                            x: 14; width: 130
                            anchors.verticalCenter: parent.verticalCenter
                            text: sysRow.modelData.label
                            kind: "label"
                        }
                        SbText {
                            id: valueText
                            objectName: "value-" + sysRow.modelData.label
                            x: 150; width: parent.width - 164
                            anchors.verticalCenter: parent.verticalCenter
                            text: sysRow.modelData.value
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: 10

            Box {
                Layout.fillWidth: true
                implicitHeight: apiCol.implicitHeight + 28
                ColumnLayout {
                    id: apiCol
                    anchors.fill: parent; anchors.margins: 14
                    spacing: 10
                    SbText { text: qsTr("Graphics APIs found"); font.weight: Font.DemiBold }
                    Flow {
                        objectName: "apiPills"
                        Layout.fillWidth: true
                        spacing: 8
                        Repeater {
                            model: AppController.apiPills
                            delegate: Pill { required property var modelData; text: modelData.text; tone: modelData.tone }
                        }
                    }
                }
            }
            Box {
                Layout.fillWidth: true
                implicitHeight: batCol.implicitHeight + 28
                ColumnLayout {
                    id: batCol
                    anchors.fill: parent; anchors.margins: 14
                    spacing: 6
                    SbText { text: qsTr("Battery"); font.weight: Font.DemiBold }
                    Repeater {
                        model: AppController.batteryLines
                        delegate: SbText {
                            required property string modelData
                            required property int index
                            objectName: "batteryLine"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: modelData
                            kind: index === 0 ? "data" : "small"
                        }
                    }
                }
            }
        }
    }
}
