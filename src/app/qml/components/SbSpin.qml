// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Syntheon

// A number field with the unit beside it (durations and the like). Values
// are real numbers; the controller clamps them to the schema's range again.
RowLayout {
    id: root

    property real from: 0
    property real to: 100
    property real stepSize: 1
    property real value: 0
    property string unit
    signal edited(real value)

    spacing: 8

    SpinBox {
        id: spin
        implicitHeight: 34
        implicitWidth: 120
        editable: true
        from: Math.round(root.from * 100)
        to: Math.round(root.to * 100)
        stepSize: Math.round(root.stepSize * 100)
        value: Math.round(root.value * 100)
        onValueModified: root.edited(value / 100)
        textFromValue: function(v) { return String(Math.round(v / 100 * 100) / 100) }
        valueFromText: function(t) { return Math.round(parseFloat(t) * 100) }
        Accessible.name: root.unit
        background: Rectangle {
            radius: Theme.radius
            color: Theme.sunk
            border.width: spin.visualFocus || spin.activeFocus ? 2 : 1
            border.color: spin.visualFocus || spin.activeFocus ? Theme.accent : Theme.line
        }
        contentItem: TextInput {
            text: spin.textFromValue(spin.value, spin.locale)
            font.family: Theme.dataFamily
            font.pixelSize: 13
            color: Theme.ink
            selectionColor: Theme.accent
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            readOnly: !spin.editable
            validator: spin.validator
            inputMethodHints: Qt.ImhFormattedNumbersOnly
        }
        up.indicator: Rectangle {
            x: spin.width - width
            height: spin.height / 2
            width: 26
            color: spin.up.pressed ? Theme.accentSoft : "transparent"
            Icon { anchors.centerIn: parent; width: 9; height: 9; path: "M3 10l5-5 5 5"; color: Theme.muted }
        }
        down.indicator: Rectangle {
            x: spin.width - width
            y: spin.height / 2
            height: spin.height / 2
            width: 26
            color: spin.down.pressed ? Theme.accentSoft : "transparent"
            Icon { anchors.centerIn: parent; width: 9; height: 9; path: "M3 6l5 5 5-5"; color: Theme.muted }
        }
    }
    SbText { visible: root.unit.length > 0; text: root.unit; kind: "small" }
}
