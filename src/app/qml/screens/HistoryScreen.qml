// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Syntheon

// Every saved run, newest first (pages of 200). Tick two or more to compare;
// Open shows a run's results.
ScreenFrame {
    id: root

    title: qsTr("History")
    subtitle: qsTr("Every run is saved on this machine. Tick two or more to compare.")

    actions: [
        SbButton {
            objectName: "compareButton"
            primary: true
            text: History.compareText
            enabled: History.canCompare
            onClicked: {
                if (Compare.setRuns(History.checkedUuids()))
                    AppController.section = "compare"
            }
        }
    ]

    Box {
        objectName: "emptyState"
        visible: History.totalCount === 0
        Layout.fillWidth: true
        implicitHeight: emptyCol.implicitHeight + 36
        ColumnLayout {
            id: emptyCol
            anchors.fill: parent
            anchors.margins: 18
            spacing: 10
            SbText { text: qsTr("No saved runs yet"); font.weight: Font.DemiBold }
            SbText {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Every run is kept here until you clear them in Settings.")
                kind: "small"
            }
            SbButton { text: qsTr("Go to Run tests"); onClicked: AppController.section = "run" }
        }
    }

    Box {
        visible: History.totalCount > 0
        Layout.fillWidth: true
        implicitHeight: Math.min(tableColumn.implicitHeight, 560)

        ColumnLayout {
            id: tableColumn
            anchors.fill: parent
            spacing: 0

            // header
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 34
                color: Theme.sunk
                radius: 8
                Rectangle { width: parent.width; height: 8; anchors.bottom: parent.bottom; color: parent.color }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14; anchors.rightMargin: 14
                    spacing: 12
                    Item { Layout.preferredWidth: 20 }
                    SbText { Layout.preferredWidth: 140; text: qsTr("Date"); kind: "label" }
                    SbText { Layout.preferredWidth: 170; text: qsTr("Machine"); kind: "label"; visible: root.width > 760 }
                    SbText { Layout.fillWidth: true; text: qsTr("Tests"); kind: "label" }
                    SbText { Layout.preferredWidth: 110; text: qsTr("Overall"); kind: "label"; horizontalAlignment: Text.AlignRight }
                    Item { Layout.preferredWidth: 64 }
                }
            }
            ListView {
                id: list
                objectName: "historyList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: Math.min(contentHeight, 520)
                implicitHeight: Math.min(contentHeight, 520)
                clip: true
                model: History
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: Item {
                    id: hrow
                    required property int index
                    required property string uuid
                    required property string date
                    required property string machine
                    required property string tests
                    required property string overall
                    required property bool partial
                    required property string status
                    required property bool checked

                    objectName: "run-" + uuid
                    width: list.width
                    height: 48

                    Rectangle {
                        visible: hrow.index < list.count - 1
                        anchors.bottom: parent.bottom
                        width: parent.width; height: 1; color: Theme.line
                    }
                    Rectangle { anchors.fill: parent; color: hover.hovered ? Theme.sunk : "transparent" }
                    HoverHandler { id: hover }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14; anchors.rightMargin: 14
                        spacing: 12
                        Rectangle {
                            objectName: "check"
                            Layout.preferredWidth: 16; Layout.preferredHeight: 16
                            radius: 3
                            color: hrow.checked ? Theme.accent : Theme.sunk
                            border.color: hrow.checked ? Theme.accent : Theme.line
                            activeFocusOnTab: true
                            Accessible.role: Accessible.CheckBox
                            Accessible.name: qsTr("Pick %1 for compare").arg(hrow.date)
                            Accessible.checked: hrow.checked
                            Keys.onSpacePressed: History.toggleChecked(hrow.uuid)
                            Icon {
                                anchors.centerIn: parent; width: 12; height: 12
                                visible: hrow.checked
                                path: "M3 8.5l3 3 7-7"; color: Theme.accentInk; strokeWidth: 2
                            }
                            TapHandler { onTapped: History.toggleChecked(hrow.uuid) }
                        }
                        SbText { Layout.preferredWidth: 140; text: hrow.date; elide: Text.ElideRight }
                        SbText {
                            Layout.preferredWidth: 170
                            visible: root.width > 760
                            text: hrow.machine
                            elide: Text.ElideRight
                        }
                        SbText { Layout.fillWidth: true; text: hrow.tests; kind: "small"; elide: Text.ElideRight }
                        RowLayout {
                            Layout.preferredWidth: 110
                            Layout.alignment: Qt.AlignRight
                            spacing: 6
                            Item { Layout.fillWidth: true }
                            Pill {
                                visible: hrow.status.length > 0
                                text: hrow.status
                                tone: hrow.status === "Recovered" ? "good" : "warn"
                            }
                            Pill { objectName: "partial"; visible: hrow.partial; text: qsTr("partial"); tone: "warn" }
                            SbText { objectName: "overall"; text: hrow.overall; kind: "data"; font.pixelSize: 13 }
                        }
                        SbButton {
                            objectName: "open"
                            Layout.preferredWidth: 64
                            text: qsTr("Open")
                            onClicked: {
                                if (Results.openFromHistory(hrow.uuid))
                                    AppController.section = "results"
                            }
                        }
                    }
                }
            }
        }
    }
}
