// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// D1: SynBench needs administrator access. Blocking: there is no limited mode,
// so the only ways out are Retry (prompt again) and Quit. While the prompt is
// open ("authorising") only Quit is offered; it also times out after two
// minutes into the failed state.
Dialog {
    id: dialog

    property bool authorising: false
    property string message
    signal retryClicked()
    signal quitClicked()

    modal: true
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 48 : 480, 480)
    padding: 18
    title: authorising ? qsTr("Waiting for administrator approval") : qsTr("SynBench needs administrator access")
    background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: 8 }
    header: SbText {
        text: dialog.title
        kind: "title"
        leftPadding: 18; topPadding: 16; rightPadding: 18
        wrapMode: Text.WordWrap
    }

    contentItem: ColumnLayout {
        spacing: 10
        SbText {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: dialog.authorising
                  ? qsTr("Approve the administrator prompt to continue. SynBench reads hardware details and runs its tests with administrator rights.")
                  : qsTr("SynBench reads hardware details and runs its tests with administrator rights. It asks once each time it starts.")
        }
        SbText {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: dialog.message.length > 0 && !dialog.authorising
            text: dialog.message
            color: Theme.bad
            kind: "small"
        }
    }

    footer: Item {
        implicitHeight: 52
        RowLayout {
            anchors.right: parent.right
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            SbButton {
                objectName: "authRetry"
                visible: !dialog.authorising
                text: qsTr("Retry")
                primary: true
                onClicked: dialog.retryClicked()
            }
            SbButton {
                objectName: "authQuit"
                text: qsTr("Quit")
                onClicked: dialog.quitClicked()
            }
        }
    }
}
