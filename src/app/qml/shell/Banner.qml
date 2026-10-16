// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import SynBench

// A persistent notice across the top of the work area (the D9 emulation
// banner).
Rectangle {
    id: root

    property string text
    property string tone: "warn"

    visible: text.length > 0
    implicitHeight: visible ? label.implicitHeight + 16 : 0
    color: Theme.sunk
    Accessible.role: Accessible.AlertMessage
    Accessible.name: text

    Rectangle { anchors.left: parent.left; width: 3; height: parent.height; color: Theme.toneColor(root.tone) }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }

    SbText {
        id: label
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        verticalAlignment: Text.AlignVCenter
        text: root.text
        wrapMode: Text.WordWrap
        kind: "small"
        color: Theme.ink
    }
}
