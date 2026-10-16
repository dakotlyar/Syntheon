// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// Pick one, several or all tests; their options appear on the right (generic,
// from each test's schema); the run bar shows the estimate and what blocks
// Start. The Battery-runs-alone rule lives in C++ (SelectionModel).
ScreenFrame {
    id: root

    title: qsTr("Run tests")
    subtitle: qsTr("Pick one, several or all. Options for each picked test appear on the right.")

    actions: [
        SbButton {
            objectName: "selectAll"
            text: qsTr("Select all")
            onClicked: RunController.selectAll()
        },
        SbButton {
            objectName: "clearSelection"
            text: qsTr("Clear")
            onClicked: RunController.clearSelection()
        }
    ]

    GridLayout {
        Layout.fillWidth: true
        columns: root.width < 860 ? 1 : 2
        columnSpacing: 16
        rowSpacing: 16

        // --- the picker ---
        Box {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: 1
            implicitHeight: pickerColumn.implicitHeight

            ColumnLayout {
                id: pickerColumn
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 0

                Repeater {
                    model: RunController.catalog
                    delegate: Rectangle {
                        id: row
                        required property int index
                        required property string testId
                        required property string name
                        required property string summary
                        required property string colorToken
                        required property bool selected
                        required property bool available
                        required property string unavailableReason
                        required property bool runsAlone

                        objectName: "pick-" + testId
                        Layout.fillWidth: true
                        implicitHeight: 52
                        color: selected ? Theme.accentSoft : (hover.hovered && available ? Theme.sunk : "transparent")
                        Accessible.role: Accessible.CheckBox
                        Accessible.name: name
                        Accessible.checked: selected
                        Accessible.checkable: true

                        Rectangle {
                            visible: row.index < RunController.catalog.rowCount() - 1
                            anchors.bottom: parent.bottom
                            width: parent.width; height: 1; color: Theme.line
                        }
                        HoverHandler { id: hover; enabled: row.available }
                        TapHandler {
                            enabled: row.available
                            onTapped: RunController.toggle(row.testId)
                        }
                        activeFocusOnTab: row.available
                        Keys.onSpacePressed: if (row.available) RunController.toggle(row.testId)
                        Keys.onReturnPressed: if (row.available) RunController.toggle(row.testId)
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 2
                            visible: row.activeFocus
                            color: "transparent"; border.color: Theme.accent; border.width: 2; radius: 4
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            spacing: 10
                            // The checkbox is drawn, the row handles the tap.
                            Rectangle {
                                objectName: "check"
                                implicitWidth: 16; implicitHeight: 16; radius: 3
                                color: row.selected ? Theme.accent : Theme.sunk
                                border.color: row.selected ? Theme.accent : Theme.line
                                opacity: row.available ? 1 : 0.5
                                Icon {
                                    anchors.centerIn: parent; width: 12; height: 12
                                    visible: row.selected
                                    path: "M3 8.5l3 3 7-7"; color: Theme.accentInk; strokeWidth: 2
                                }
                            }
                            Swatch {
                                implicitHeight: 30
                                color: row.available ? Theme.testColor(row.colorToken) : Theme.na
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                SbText {
                                    text: row.name
                                    font.weight: Font.DemiBold
                                    color: row.available ? Theme.ink : Theme.na
                                }
                                SbText {
                                    Layout.fillWidth: true
                                    text: row.available ? row.summary : row.unavailableReason
                                    kind: "small"
                                    elide: Text.ElideRight
                                    color: row.available ? Theme.muted : Theme.na
                                }
                            }
                            Pill {
                                visible: !row.available
                                text: qsTr("N/A")
                                tone: "na"
                            }
                            Pill {
                                visible: row.available && row.runsAlone
                                text: qsTr("Runs alone")
                                tone: "warn"
                            }
                            Pill {
                                visible: row.available && !row.runsAlone
                                text: { RunController.estimateText; return RunController.estimateOf(row.testId) }
                            }
                        }
                    }
                }
            }
        }

        // --- option tabs ---
        Box {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: 1
            implicitHeight: optionsColumn.implicitHeight

            ColumnLayout {
                id: optionsColumn
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 0

                // Tabs, one per ticked test.
                Flow {
                    objectName: "optionTabs"
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    Layout.leftMargin: 10
                    Layout.rightMargin: 10
                    spacing: 4
                    visible: RunController.tabs.length > 0
                    Repeater {
                        model: RunController.tabs
                        delegate: AbstractButton {
                            id: tab
                            required property var modelData
                            readonly property bool active: modelData.id === RunController.activeTab
                            objectName: "tab-" + modelData.id
                            text: modelData.name
                            implicitHeight: 30
                            leftPadding: 10; rightPadding: 10
                            activeFocusOnTab: true
                            Accessible.role: Accessible.PageTab
                            Accessible.name: text
                            Accessible.selected: active
                            onClicked: RunController.activeTab = modelData.id
                            background: Rectangle {
                                color: tab.active ? Theme.panel : "transparent"
                                border.color: tab.active ? Theme.line : "transparent"
                                radius: 6
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: tab.active ? 2 : 0; color: Theme.panel }
                            }
                            contentItem: RowLayout {
                                spacing: 6
                                Rectangle { width: 7; height: 7; radius: 3.5; color: Theme.testColor(tab.modelData.colorToken) }
                                SbText {
                                    text: tab.text
                                    kind: "small"
                                    font.weight: Font.DemiBold
                                    color: tab.active ? Theme.ink : Theme.muted
                                }
                            }
                        }
                    }
                }
                Rectangle { visible: RunController.tabs.length > 0; Layout.fillWidth: true; height: 1; color: Theme.line }

                SbText {
                    visible: RunController.tabs.length === 0
                    Layout.margins: 14
                    text: qsTr("Tick a test on the left to see its options.")
                    kind: "muted"
                }
                OptionTab {
                    visible: RunController.tabs.length > 0
                    Layout.fillWidth: true
                    Layout.margins: 14
                    optionsModel: RunController.options
                    preconditions: RunController.preconditions
                    onRefreshRequested: RunController.refreshProbes()
                }
            }
        }
    }

    // --- the run bar ---
    Box {
        Layout.fillWidth: true
        implicitHeight: runBar.implicitHeight + 24

        ColumnLayout {
            id: runBar
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    SbText {
                        objectName: "selectedCount"
                        text: RunController.selectedCount === 1 ? qsTr("1 test selected")
                              : qsTr("%1 tests selected").arg(RunController.selectedCount)
                        font.weight: Font.DemiBold
                    }
                    SbText {
                        objectName: "estimate"
                        visible: RunController.estimateText.length > 0
                        text: "· " + RunController.estimateText
                        kind: "small"
                    }
                }
                SbButton {
                    objectName: "runButton"
                    primary: true
                    text: RunController.runButtonText
                    enabled: RunController.canStart
                    onClicked: RunController.start()
                }
            }
            // Start was pressed and refused: say why.
            Repeater {
                model: RunController.startErrors
                delegate: SbText {
                    required property string modelData
                    objectName: "startError"
                    Layout.fillWidth: true
                    text: modelData
                    kind: "small"
                    color: Theme.bad
                    wrapMode: Text.WordWrap
                }
            }
            // What is blocking Start, in words.
            Repeater {
                model: RunController.selectedCount > 0 ? RunController.problems : []
                delegate: SbText {
                    required property string modelData
                    objectName: "problem"
                    Layout.fillWidth: true
                    text: modelData
                    kind: "small"
                    color: Theme.warn
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
