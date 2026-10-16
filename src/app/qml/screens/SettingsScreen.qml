// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import SynBench

// Layout, theme, data location (move or open an existing history, D7) and
// Clear history. Changes apply right away.
ScreenFrame {
    id: root

    title: qsTr("Settings")
    subtitle: qsTr("Changes apply right away")

    property string pendingFolder     // a folder that already holds a history (D7)
    property string message           // result of the last location change
    property bool messageIsError: false
    property bool confirmingClear: false

    function showMessage(text, error) {
        message = text
        messageIsError = error
    }

    // Called with the folder the user picked.
    function folderChosen(path) {
        pendingFolder = ""
        const kind = StorageController.inspectFolder(path)
        if (kind === "database") {
            pendingFolder = path
        } else if (kind === "empty") {
            const result = StorageController.moveHistoryTo(path)
            if (result === "")
                showMessage(qsTr("The history was moved."), false)
            else if (result.indexOf("warning:") === 0)
                showMessage(qsTr("The history was moved. %1").arg(result.substring(8)), false)
            else
                showMessage(result, true)
        } else {
            showMessage(qsTr("That folder can't be used: it doesn't exist or isn't writable."), true)
        }
    }

    FolderDialog {
        id: picker
        title: qsTr("Choose where to keep your history")
        onAccepted: root.folderChosen(StorageController.urlToPath(selectedFolder))
    }

    Box {
        Layout.fillWidth: true
        implicitHeight: settingsColumn.implicitHeight

        ColumnLayout {
            id: settingsColumn
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0

            // --- Navigation layout ---
            GridLayout {
                Layout.fillWidth: true
                Layout.margins: 14
                columns: root.width < 640 ? 1 : 2
                columnSpacing: 20
                rowSpacing: 8
                ColumnLayout {
                    Layout.preferredWidth: 220
                    spacing: 2
                    SbText { text: qsTr("Navigation layout"); font.weight: Font.DemiBold }
                    SbText { text: qsTr("Where the section list sits"); kind: "small" }
                }
                Segmented {
                    objectName: "layoutSegmented"
                    accessibleName: qsTr("Navigation layout")
                    items: [{ value: "rail", label: qsTr("Left rail") }, { value: "tabs", label: qsTr("Top tabs") }]
                    current: Settings.layout
                    onActivated: value => Settings.layout = value
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }

            // --- Theme ---
            GridLayout {
                Layout.fillWidth: true
                Layout.margins: 14
                columns: root.width < 640 ? 1 : 2
                columnSpacing: 20
                rowSpacing: 8
                ColumnLayout {
                    Layout.preferredWidth: 220
                    spacing: 2
                    SbText { text: qsTr("Theme"); font.weight: Font.DemiBold }
                    SbText { text: qsTr("System follows your OS setting"); kind: "small" }
                }
                Segmented {
                    objectName: "themeSegmented"
                    accessibleName: qsTr("Theme")
                    items: [{ value: "system", label: qsTr("System") }, { value: "light", label: qsTr("Light") },
                            { value: "dark", label: qsTr("Dark") }]
                    current: Settings.theme
                    onActivated: value => Settings.theme = value
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }

            // --- Data location ---
            GridLayout {
                Layout.fillWidth: true
                Layout.margins: 14
                columns: root.width < 640 ? 1 : 2
                columnSpacing: 20
                rowSpacing: 8
                ColumnLayout {
                    Layout.preferredWidth: 220
                    Layout.alignment: Qt.AlignTop
                    spacing: 2
                    SbText { text: qsTr("Data location"); font.weight: Font.DemiBold }
                    SbText {
                        text: qsTr("Where run history is saved on this machine")
                        kind: "small"
                        wrapMode: Text.WordWrap
                        Layout.preferredWidth: 220
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 34
                            radius: Theme.radius
                            color: Theme.sunk
                            border.color: Theme.line
                            SbText {
                                objectName: "dataPath"
                                anchors.fill: parent
                                anchors.leftMargin: 9
                                anchors.rightMargin: 9
                                verticalAlignment: Text.AlignVCenter
                                text: StorageController.dbPath
                                kind: "data"
                                font.pixelSize: 13
                                elide: Text.ElideMiddle
                            }
                        }
                        SbButton {
                            objectName: "changeLocation"
                            text: qsTr("Change…")
                            enabled: !StorageController.locked
                            onClicked: picker.open()
                        }
                    }
                    SbText {
                        visible: StorageController.locked
                        text: StorageController.lockedReason
                        kind: "small"
                        color: Theme.warn
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    SbText {
                        objectName: "historyHint"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        kind: "small"
                        text: (StorageController.runCount === 1 ? qsTr("1 saved run")
                               : qsTr("%1 saved runs").arg(StorageController.runCount))
                              + " · " + StorageController.sizeText + qsTr(" · kept until you clear them")
                    }

                    // D7: the folder already has a history.
                    Rectangle {
                        objectName: "openExistingBox"
                        visible: root.pendingFolder.length > 0
                        Layout.fillWidth: true
                        implicitHeight: openCol.implicitHeight + 20
                        radius: Theme.radius
                        color: "transparent"
                        border.color: Theme.accent
                        ColumnLayout {
                            id: openCol
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 8
                            SbText {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: qsTr("That folder already has a SynBench history. Open it? Your current history "
                                           + "stays where it is, and the two are never merged.")
                            }
                            RowLayout {
                                spacing: 6
                                SbButton {
                                    objectName: "openExisting"
                                    text: qsTr("Open it")
                                    primary: true
                                    onClicked: {
                                        const result = StorageController.openHistoryIn(root.pendingFolder)
                                        root.pendingFolder = ""
                                        root.showMessage(result === "" ? qsTr("Switched to that history.") : result, result !== "")
                                    }
                                }
                                SbButton { text: qsTr("Cancel"); onClicked: root.pendingFolder = "" }
                            }
                        }
                    }
                    SbText {
                        objectName: "locationMessage"
                        visible: root.message.length > 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        kind: "small"
                        color: root.messageIsError ? Theme.bad : Theme.good
                        text: root.message
                    }

                    RowLayout {
                        spacing: 8
                        SbButton {
                            objectName: "clearHistory"
                            text: qsTr("Clear history…")
                            danger: true
                            enabled: !StorageController.locked && StorageController.runCount > 0
                            onClicked: root.confirmingClear = true
                        }
                    }
                    Rectangle {
                        objectName: "clearConfirm"
                        visible: root.confirmingClear
                        Layout.fillWidth: true
                        implicitHeight: clearCol.implicitHeight + 20
                        radius: Theme.radius
                        color: "transparent"
                        border.color: Theme.bad
                        ColumnLayout {
                            id: clearCol
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 8
                            SbText {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: (StorageController.runCount === 1 ? qsTr("Delete the saved run from this machine?")
                                       : qsTr("Delete all %1 saved runs from this machine?").arg(StorageController.runCount))
                                      + " " + qsTr("This can't be undone.")
                            }
                            RowLayout {
                                spacing: 6
                                SbButton {
                                    objectName: "clearYes"
                                    text: StorageController.runCount === 1 ? qsTr("Delete 1 run")
                                          : qsTr("Delete %1 runs").arg(StorageController.runCount)
                                    danger: true
                                    onClicked: {
                                        const result = StorageController.clearHistory()
                                        root.confirmingClear = false
                                        root.showMessage(result === "" ? qsTr("History cleared.") : result, result !== "")
                                    }
                                }
                                SbButton { text: qsTr("Cancel"); onClicked: root.confirmingClear = false }
                            }
                        }
                    }
                }
            }
        }
    }
}
