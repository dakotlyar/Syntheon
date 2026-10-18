// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// Settings > Licences (D11): THIRD-PARTY-NOTICES.txt, the notices of the
// libraries and fonts SynBench includes, as shipped beside LICENSE.txt.
SbDialog {
    id: dialog

    // Sized and centred on the window, wherever it is declared (Settings
    // declares it inside its content column).
    parent: Overlay.overlay

    width: Math.min(parent ? parent.width - 48 : 760, 760)
    height: Math.min(parent ? parent.height - 48 : 560, 560)
    padding: 16

    title: qsTr("Licences")
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    contentItem: ColumnLayout {
        spacing: 10
        SbText {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            kind: "small"
            text: qsTr("SynBench is free software under the GNU GPL, version 3 or later. It includes the software below, "
                       + "each under its own licence.")
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.sunk
            radius: 6
            Flickable {
                id: flick
                anchors.fill: parent
                anchors.margins: 10
                clip: true
                contentWidth: width
                contentHeight: notices.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                TextEdit {
                    id: notices
                    objectName: "noticesText"
                    width: flick.width - 12
                    readOnly: true
                    selectByMouse: true
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                    text: AppController.thirdPartyNotices.length > 0 ? AppController.thirdPartyNotices
                                                                     : qsTr("The notices aren't available in this build.")
                    font.family: Theme.dataFamily
                    font.pixelSize: 12
                    color: Theme.ink
                }
            }
        }
    }
    footer: Item {
        implicitHeight: 52
        SbButton {
            objectName: "closeLicences"
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    }
}
