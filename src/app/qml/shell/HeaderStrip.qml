// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import SynBench

// D18: the OS draws the title bar; this slim strip inside the window carries
// the brand, the machine and the power state.
Rectangle {
    id: root

    property string machine
    property string detail

    implicitHeight: 40
    color: Theme.panel

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 8

        // The speedometer mark from assets/synbench.svg.
        Item {
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
            Icon {
                anchors.fill: parent
                box: 24
                strokeWidth: 2
                color: Theme.ink
                path: "M3.5 16a8.5 8.5 0 1 1 17 0"
            }
            Icon {
                anchors.fill: parent
                box: 24
                strokeWidth: 2.2
                color: Theme.accent
                path: "M12 16l4.5-5"
            }
            Rectangle {
                x: parent.width * 12 / 24 - 2
                y: parent.height * 16 / 24 - 2
                width: 4; height: 4; radius: 2
                color: Theme.accent
            }
        }
        SbText { text: qsTr("SynBench"); kind: "display"; font.pixelSize: 15 }
        SbText {
            objectName: "headerMachine"
            Layout.fillWidth: true
            Layout.leftMargin: 6
            text: root.machine.length > 0 ? root.machine : qsTr("Detecting hardware…")
            kind: "small"
            elide: Text.ElideRight
        }
        SbText {
            objectName: "headerDetail"
            visible: root.width > 640
            text: root.detail
            kind: "small"
            elide: Text.ElideRight
            Layout.maximumWidth: root.width * 0.45
        }
    }
}
