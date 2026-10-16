// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import SynBench

Rectangle {
    id: root

    property real value: 0  // 0-1
    property color fill: Theme.accent

    implicitHeight: 8
    radius: 4
    color: Theme.sunk
    border.color: Theme.line
    border.width: 1
    clip: true
    Accessible.role: Accessible.ProgressBar
    Accessible.name: qsTr("Progress")
    Accessible.description: Math.round(value * 100) + "%"

    Rectangle {
        width: parent.width * Math.max(0, Math.min(1, root.value))
        height: parent.height
        color: root.fill
        Behavior on width { NumberAnimation { duration: 120 } }
    }
}
