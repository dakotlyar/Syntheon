// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Syntheon

// The generic option tab of one test: every option row from its schema, then
// (Battery) the pre-start checks.
ColumnLayout {
    id: root

    property var optionsModel          // an OptionsModel, or null while probing
    property var preconditions: []     // [{id, label, state: met|notmet|apply, detail}]
    signal refreshRequested()

    spacing: 12

    SbText {
        visible: root.optionsModel === null
        text: qsTr("Detecting what this machine supports…")
        kind: "muted"
    }

    Repeater {
        model: root.optionsModel
        delegate: OptionEditor {
            Layout.fillWidth: true
            optionsModel: root.optionsModel
            onRefreshRequested: root.refreshRequested()
        }
    }

    // Live pre-start checks ("On AC: unplug", "100%", "Will lock at start").
    ColumnLayout {
        objectName: "preconditions"
        visible: root.preconditions.length > 0
        Layout.fillWidth: true
        spacing: 5
        SbText { text: qsTr("Before it can start"); kind: "label" }
        Repeater {
            model: root.preconditions
            delegate: Rectangle {
                id: precondition
                required property var modelData
                objectName: "precondition-" + precondition.modelData.id
                Layout.fillWidth: true
                implicitHeight: row.implicitHeight + 12
                radius: Theme.radius
                color: Theme.sunk
                RowLayout {
                    id: row
                    anchors.fill: parent
                    anchors.margins: 6
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    SbText { text: precondition.modelData.label; Layout.fillWidth: true }
                    Pill {
                        text: precondition.modelData.detail
                        tone: precondition.modelData.state === "met" ? "good" : precondition.modelData.state === "notmet" ? "bad" : "good"
                    }
                }
            }
        }
    }
}
