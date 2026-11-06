// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Syntheon

// The same sections as a strip across the top (Settings toggle, or a narrow
// window).
Rectangle {
    id: root

    property var sections: []
    property string current
    property string version
    signal navigate(string id)
    // Under 900 px the tabs tighten (less padding and spacing), so all seven
    // fit at the 760 px minimum (Zeb: Settings was cut off).
    readonly property bool compact: width < 900

    implicitHeight: 44
    color: Theme.win

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }

    Flickable {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        contentWidth: row.implicitWidth
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        RowLayout {
            id: row
            height: parent.height
            spacing: root.compact ? 0 : 2

            Repeater {
                model: root.sections
                delegate: Item {
                    id: entry
                    required property var modelData
                    Layout.fillHeight: true
                    Layout.fillWidth: entry.modelData.spacer === true
                    Layout.minimumWidth: entry.modelData.spacer === true ? 8 : btn.implicitWidth
                    implicitWidth: entry.modelData.spacer === true ? 8 : btn.implicitWidth

                    AbstractButton {
                        id: btn
                        objectName: "nav-" + (entry.modelData.id || "")
                        visible: !entry.modelData.spacer
                        anchors.verticalCenter: parent.verticalCenter
                        height: 34
                        padding: 0
                        activeFocusOnTab: true
                        hoverEnabled: true
                        text: entry.modelData.label || ""
                        Accessible.role: Accessible.PageTab
                        Accessible.name: text
                        Accessible.selected: entry.modelData.id === root.current
                        onClicked: root.navigate(entry.modelData.id)
                        Keys.onReturnPressed: root.navigate(entry.modelData.id)
                        background: Rectangle {
                            radius: Theme.radius
                            color: entry.modelData.id === root.current ? Theme.accentSoft : (btn.hovered ? Theme.sunk : "transparent")
                            border.width: btn.visualFocus ? 2 : 0
                            border.color: Theme.accent
                        }
                        contentItem: RowLayout {
                            spacing: root.compact ? 5 : 8
                            Item { Layout.preferredWidth: root.compact ? 1 : 2 }
                            Icon {
                                path: entry.modelData.icon || ""
                                color: entry.modelData.id === root.current ? Theme.accent : Theme.muted
                            }
                            SbText {
                                text: btn.text
                                font.weight: Font.Medium
                                color: entry.modelData.id === root.current ? Theme.ink : Theme.muted
                            }
                            Item { Layout.preferredWidth: root.compact ? 3 : 6 }
                        }
                    }
                }
            }
        }
    }
}
