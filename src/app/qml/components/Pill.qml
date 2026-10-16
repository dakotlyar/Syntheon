// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import SynBench

// A small status pill. Colour is never the only signal: the text always says it.
//   tone: "" | "good" | "warn" | "bad" | "na" | "acc"
Rectangle {
    id: root

    property string text
    property string tone: ""

    implicitWidth: label.implicitWidth + 14
    implicitHeight: label.implicitHeight + 6
    radius: height / 2
    color: "transparent"
    border.width: 1
    border.color: tone === "" ? Theme.line : Theme.toneColor(tone)
    Accessible.role: Accessible.StaticText
    Accessible.name: text

    SbText {
        id: label
        anchors.centerIn: parent
        text: root.text
        kind: "data"
        font.pixelSize: 11
        color: root.tone === "" ? Theme.muted : Theme.toneColor(root.tone)
    }
}
