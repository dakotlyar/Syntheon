// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import SynBench

// §4: the saved runs can't be opened at launch (a removed drive, or a history
// from a newer SynBench). Use the default location, choose another, or quit.
// Nothing is ever downgraded or overwritten.
SbDialog {
    id: dialog

    property string message
    property bool newerVersion: false
    property string error
    signal useDefault()
    signal chooseFolder(string path)
    signal quitClicked()

    width: Math.min(parent ? parent.width - 48 : 520, 520)
    title: newerVersion ? qsTr("These saved runs are from a newer SynBench") : qsTr("Your saved runs aren't available")

    FolderDialog {
        id: picker
        title: qsTr("Choose a folder for your history")
        onAccepted: dialog.chooseFolder(StorageController.urlToPath(selectedFolder))
    }

    contentItem: ColumnLayout {
        spacing: 10
        SbText {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: dialog.message
        }
        SbText {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: dialog.error.length > 0
            text: dialog.error
            color: Theme.bad
            kind: "small"
        }
    }

    footer: DialogButtonBox {
        background: Item {}
        SbButton { text: qsTr("Use default location"); onClicked: dialog.useDefault() }
        SbButton { text: qsTr("Choose…"); onClicked: picker.open() }
        SbButton { text: qsTr("Quit"); onClicked: dialog.quitClicked() }
    }
}
