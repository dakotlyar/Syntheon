// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// The mockup's segmented button. `items` = [{value, label, enabled?}].
Rectangle {
    id: root

    property var items: []
    property string current
    property string accessibleName
    signal activated(string value)

    implicitWidth: row.implicitWidth
    implicitHeight: 30
    radius: Theme.radius
    color: "transparent"
    border.color: Theme.line
    border.width: 1
    clip: true
    Accessible.role: Accessible.RadioButton
    Accessible.name: accessibleName

    RowLayout {
        id: row
        anchors.fill: parent
        spacing: 0

        Repeater {
            model: root.items
            delegate: AbstractButton {
                id: seg
                required property var modelData
                required property int index
                readonly property bool on: modelData.value === root.current
                Layout.fillHeight: true
                implicitWidth: label.implicitWidth + 24
                enabled: modelData.enabled !== false
                text: modelData.label
                checkable: false
                activeFocusOnTab: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: text
                Accessible.checked: on
                onClicked: root.activated(modelData.value)
                Keys.onReturnPressed: root.activated(modelData.value)
                Keys.onSpacePressed: root.activated(modelData.value)

                background: Rectangle {
                    color: seg.on ? Theme.accent : "transparent"
                    opacity: seg.enabled ? 1 : 0.5
                    Rectangle {
                        visible: seg.index < root.items.length - 1
                        anchors.right: parent.right
                        width: 1
                        height: parent.height
                        color: Theme.line
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        visible: seg.visualFocus
                        color: "transparent"
                        border.color: seg.on ? Theme.accentInk : Theme.accent
                        border.width: 2
                        radius: 3
                    }
                }
                contentItem: Text {
                    id: label
                    text: seg.text
                    font.family: Theme.bodyFamily
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: seg.on ? Theme.accentInk : Theme.muted
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
}
