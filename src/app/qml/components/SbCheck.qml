// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import Syntheon

// A checkbox in the mockup's look (accent when on). Keeps its reason when
// disabled ("OpenVINO (no Intel device)").
CheckBox {
    id: root

    spacing: 8
    padding: 2
    hoverEnabled: true
    Accessible.name: text

    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        x: root.leftPadding
        y: root.topPadding + (root.availableHeight - height) / 2
        radius: 3
        color: root.checked ? Theme.accent : Theme.sunk
        border.color: root.visualFocus ? Theme.accent : (root.checked ? Theme.accent : Theme.line)
        border.width: root.visualFocus ? 2 : 1
        opacity: root.enabled ? 1 : 0.5

        Icon {
            anchors.centerIn: parent
            visible: root.checked
            width: 12
            height: 12
            path: "M3 8.5l3 3 7-7"
            color: Theme.accentInk
            strokeWidth: 2
        }
    }
    contentItem: Text {
        leftPadding: root.indicator.width + root.spacing
        text: root.text
        font.family: Theme.bodyFamily
        font.pixelSize: 13
        color: root.enabled ? Theme.ink : Theme.na
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
