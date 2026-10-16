// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import SynBench

// Mockup buttons: plain, primary (accent) and danger.
Button {
    id: root

    property bool primary: false
    property bool danger: false

    implicitHeight: 32
    leftPadding: 12
    rightPadding: 12
    hoverEnabled: true
    Accessible.name: text

    contentItem: Text {
        text: root.text
        font.family: Theme.bodyFamily
        font.pixelSize: 13
        font.weight: root.primary ? Font.DemiBold : Font.Medium
        color: root.primary ? Theme.accentInk : root.danger ? Theme.bad : Theme.ink
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Theme.radius
        color: root.primary ? Theme.accent : Theme.panel
        opacity: root.enabled ? 1 : 0.5
        border.width: root.visualFocus ? 2 : 1
        border.color: root.visualFocus || root.hovered ? Theme.accent : (root.primary ? Theme.accent : Theme.line)
    }
}
