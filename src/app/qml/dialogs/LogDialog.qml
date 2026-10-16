// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// A stored run's log (D16), filterable by test and level, with Copy. Works for
// any run in History, including Stopped, Interrupted and Recovered ones.
Dialog {
    id: dialog

    property var model: null

    function openLog() {
        model = Results.logModel()
        testFilter.current = ""
        levelFilter.current = ""
        open()
    }

    modal: true
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 48 : 760, 760)
    height: Math.min(parent ? parent.height - 48 : 520, 520)
    padding: 16
    background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: 8 }
    header: SbText { text: qsTr("Run log"); kind: "title"; leftPadding: 16; topPadding: 14 }

    contentItem: ColumnLayout {
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            SbCombo {
                id: testFilter
                objectName: "logTestFilter"
                Layout.preferredWidth: 200
                items: [{ value: "", label: qsTr("All tests") }].concat(Results.logTests())
                onChosen: value => { current = value; if (dialog.model) dialog.model.testFilter = value }
            }
            SbCombo {
                id: levelFilter
                objectName: "logLevelFilter"
                Layout.preferredWidth: 160
                items: [{ value: "", label: qsTr("All levels") }, { value: "warning", label: qsTr("Warnings and errors") },
                        { value: "error", label: qsTr("Errors only") }]
                onChosen: value => { current = value; if (dialog.model) dialog.model.minLevel = value }
            }
            Item { Layout.fillWidth: true }
            SbText { text: qsTr("%1 lines").arg(dialog.model ? dialog.model.rowCount() : 0); kind: "small" }
            SbButton {
                objectName: "copyLog"
                text: qsTr("Copy")
                onClicked: {
                    clip.text = dialog.model ? dialog.model.visibleText() : ""
                    clip.selectAll()
                    clip.copy()
                }
            }
        }
        TextEdit { id: clip; visible: false }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.sunk
            radius: 6
            ListView {
                id: view
                objectName: "storedLogView"
                anchors.fill: parent
                anchors.margins: 10
                clip: true
                model: dialog.model
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: SbText {
                    required property string time
                    required property string testName
                    required property string message
                    required property string level
                    width: view.width - 12
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    text: time + "  " + (testName.length > 0 ? testName.padEnd(8) : "        ") + message
                    kind: "data"; font.pixelSize: 12
                    color: level === "error" ? Theme.bad : level === "warning" ? Theme.warn : Theme.ink
                }
                SbText {
                    visible: view.count === 0
                    anchors.centerIn: parent
                    text: qsTr("No log lines for this run.")
                    kind: "muted"
                }
            }
        }
    }
    footer: Item {
        implicitHeight: 52
        SbButton {
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    }
}
