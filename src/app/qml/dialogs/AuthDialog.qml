// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// D1: SynBench needs administrator access. Blocking: no limited mode, so the
// only ways out are Retry (prompt again) and Quit. Styled properly with the
// QML foundation (task 12).
Dialog {
    id: dialog

    property string message
    signal retryClicked()
    signal quitClicked()

    modal: true
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 48 : 480, 480)
    title: qsTr("SynBench needs administrator access")

    contentItem: ColumnLayout {
        spacing: 12
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("SynBench reads hardware details and runs its tests with administrator rights. "
                       + "It asks once each time it starts.")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: dialog.message.length > 0
            text: dialog.message
            font.italic: true
        }
    }

    footer: DialogButtonBox {
        Button {
            text: qsTr("Retry")
            Accessible.name: text
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: dialog.retryClicked()
        }
        Button {
            text: qsTr("Quit")
            Accessible.name: text
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: dialog.quitClicked()
        }
    }
}
