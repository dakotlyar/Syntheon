// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import Syntheon

// A single-line text field in the mockup's look.
TextField {
    id: root

    implicitHeight: 34
    leftPadding: 9
    rightPadding: 9
    font.family: Theme.dataFamily
    font.pixelSize: 13
    color: Theme.ink
    placeholderTextColor: Theme.muted
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentInk
    Accessible.name: placeholderText
    background: Rectangle {
        radius: Theme.radius
        color: Theme.sunk
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? Theme.accent : Theme.line
    }
}
