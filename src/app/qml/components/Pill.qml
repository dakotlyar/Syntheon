// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Syntheon

// A small status pill. Colour is never the only signal: the text always says it.
//   tone: "" | "good" | "warn" | "bad" | "na" | "acc"
// A pill never draws outside the width it is given: give it a width (or a
// Layout.maximumWidth) and long text wraps onto more lines inside it.
Rectangle {
    id: root

    property string text
    property string tone: ""
    readonly property real naturalWidth: measure.implicitWidth + 14

    implicitWidth: naturalWidth
    implicitHeight: label.implicitHeight + 6
    // Round ends for one line; a softer rectangle when the text wraps.
    radius: label.lineCount > 1 ? Theme.radius : height / 2
    color: "transparent"
    border.width: 1
    border.color: tone === "" ? Theme.line : Theme.toneColor(tone)
    clip: true
    Accessible.role: Accessible.StaticText
    Accessible.name: text

    // The one-line width, measured without wrapping.
    SbText {
        id: measure
        visible: false
        text: root.text
        kind: "data"
        font.pixelSize: 11
    }
    SbText {
        id: label
        objectName: "pillText"
        x: 7
        y: 3
        width: Math.max(0, root.width - 14)
        text: root.text
        kind: "data"
        font.pixelSize: 11
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        color: root.tone === "" ? Theme.muted : Theme.toneColor(root.tone)
    }
}
