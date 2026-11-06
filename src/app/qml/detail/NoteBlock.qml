// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Syntheon

// A short note with a coloured edge ("Scene contents are decided in Phase 5").
Item {
    id: root

    property var block

    implicitHeight: text.implicitHeight + 6

    Rectangle {
        width: 2; height: parent.height
        color: root.block ? Theme.toneColor(root.block.tone === "neutral" ? "warn" : root.block.tone) : Theme.warn
    }
    SbText {
        id: text
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: root.block ? root.block.text : ""
        kind: "small"
        wrapMode: Text.WordWrap
    }
}
