// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import SynBench

// One option, drawn by its kind (P7): the one generic renderer. Reads the
// OptionsModel's roles and writes through `optionsModel.setValue`.
ColumnLayout {
    id: root

    required property var optionsModel
    required property string key
    required property string label
    required property string kind        // bool | multi | single | dynamic | number | path | info
    required property string help
    required property var value
    required property var items
    required property bool segmented
    required property real minimum
    required property real maximum
    required property real step
    required property string unit
    required property string infoValue
    required property string infoTone
    signal refreshRequested()

    objectName: "option-" + key
    spacing: 5

    function edit(v) { optionsModel.setValue(key, v) }

    // The label above the control (a checkbox carries its own).
    SbText {
        visible: root.kind !== "bool" && root.kind !== "info"
        text: root.label
        kind: "label"
    }

    // --- Bool ---
    SbCheck {
        objectName: "bool"
        visible: root.kind === "bool"
        text: root.label
        checked: root.kind === "bool" && root.value === true
        onToggled: root.edit(checked)
    }

    // --- MultiSelect: a checkbox group; items can be disabled with a reason ---
    Flow {
        visible: root.kind === "multi"
        Layout.fillWidth: true
        spacing: 6
        Repeater {
            model: root.kind === "multi" ? root.items : []
            delegate: SbCheck {
                required property var modelData
                objectName: "multi-" + modelData.value
                enabled: modelData.enabled
                // The reason once: an item whose label already says it isn't repeated.
                text: modelData.enabled || !modelData.disabledReason
                      || modelData.label.indexOf(modelData.disabledReason) >= 0
                      ? modelData.label : modelData.label + " (" + modelData.disabledReason + ")"
                checked: root.value.indexOf(modelData.value) >= 0
                onToggled: {
                    const list = root.value.slice()
                    const at = list.indexOf(modelData.value)
                    if (checked && at < 0) list.push(modelData.value)
                    if (!checked && at >= 0) list.splice(at, 1)
                    root.edit(list)
                }
            }
        }
    }

    // --- SingleSelect (segmented or dropdown) and DynamicSelect ---
    Segmented {
        objectName: "segmented"
        visible: root.kind === "single" && root.segmented
        accessibleName: root.label
        items: root.kind === "single" ? root.items : []
        current: root.value
        onActivated: value => root.edit(value)
    }
    RowLayout {
        visible: (root.kind === "single" && !root.segmented) || root.kind === "dynamic"
        Layout.fillWidth: true
        spacing: 6
        SbCombo {
            objectName: "combo"
            Layout.fillWidth: true
            items: root.kind === "single" || root.kind === "dynamic" ? root.items : []
            current: root.value
            onChosen: value => root.edit(value)
        }
        SbButton {
            objectName: "refresh"
            visible: root.kind === "dynamic"
            text: qsTr("Refresh")
            onClicked: root.refreshRequested()
        }
    }
    SbText {
        visible: root.kind === "dynamic" && root.items.length === 0
        text: qsTr("Nothing found.")
        kind: "small"
    }

    // --- Number ---
    SbSpin {
        objectName: "number"
        visible: root.kind === "number"
        from: root.minimum
        to: root.maximum
        stepSize: root.step
        value: root.kind === "number" ? root.value : 0
        unit: root.unit
        onEdited: v => root.edit(v)
    }

    // --- Path ---
    RowLayout {
        visible: root.kind === "path"
        Layout.fillWidth: true
        spacing: 6
        SbField {
            id: pathField
            objectName: "path"
            Layout.fillWidth: true
            placeholderText: qsTr("Default folder")
            text: root.kind === "path" ? root.value : ""
            onEditingFinished: root.edit(text)
        }
        SbButton {
            text: qsTr("Choose…")
            onClicked: folderDialog.open()
        }
        FolderDialog {
            id: folderDialog
            title: root.label
            onAccepted: {
                const p = selectedFolder.toString().replace(/^file:\/\/(?=\/[A-Za-z]:|\/)/, "")
                pathField.text = decodeURIComponent(p)
                root.edit(pathField.text)
            }
        }
    }

    // --- Info: label, value, pill ---
    RowLayout {
        objectName: "info"
        visible: root.kind === "info"
        Layout.fillWidth: true
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: infoRow.implicitHeight + 12
            radius: Theme.radius
            color: Theme.sunk
            RowLayout {
                id: infoRow
                anchors.fill: parent
                anchors.margins: 6
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                SbText {
                    objectName: "infoLabel"
                    text: root.label
                    Layout.fillWidth: root.infoTone !== "neutral"
                    Layout.minimumWidth: implicitWidth
                    Layout.alignment: Qt.AlignTop
                }
                Pill {
                    objectName: "infoPill"
                    visible: root.infoTone !== "neutral"
                    text: root.infoValue
                    tone: root.infoTone === "neutral" ? "" : root.infoTone
                    // Never wider than the room the label leaves (it wraps instead
                    // of spilling over the label, whatever the tone).
                    Layout.maximumWidth: Math.max(60, infoRow.width - infoRow.spacing - 80)
                    Layout.preferredWidth: Math.min(naturalWidth, Layout.maximumWidth)
                    Layout.alignment: Qt.AlignRight | Qt.AlignTop
                }
                SbText {
                    visible: root.infoTone === "neutral"
                    text: root.infoValue
                    kind: "data"
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignRight
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                }
            }
        }
    }

    SbText {
        visible: root.help.length > 0 && root.kind !== "info"
        Layout.fillWidth: true
        text: root.help
        kind: "small"
        wrapMode: Text.WordWrap
    }
}
