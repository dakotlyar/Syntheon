// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// The same sections as a strip across the top (Settings toggle, or a narrow
// window).
Rectangle {
    id: root

    property var sections: []
    property string current
    property string version
    signal navigate(string id)

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
            spacing: 2

            Repeater {
                model: root.sections
                delegate: Item {
                    required property var modelData
                    Layout.fillHeight: true
                    Layout.fillWidth: modelData.spacer === true
                    Layout.minimumWidth: modelData.spacer === true ? 8 : btn.implicitWidth
                    implicitWidth: modelData.spacer === true ? 8 : btn.implicitWidth

                    AbstractButton {
                        id: btn
                        objectName: "nav-" + (modelData.id || "")
                        visible: !modelData.spacer
                        anchors.verticalCenter: parent.verticalCenter
                        height: 34
                        padding: 0
                        activeFocusOnTab: true
                        hoverEnabled: true
                        text: modelData.label || ""
                        Accessible.role: Accessible.PageTab
                        Accessible.name: text
                        Accessible.selected: modelData.id === root.current
                        onClicked: root.navigate(modelData.id)
                        Keys.onReturnPressed: root.navigate(modelData.id)
                        background: Rectangle {
                            radius: Theme.radius
                            color: modelData.id === root.current ? Theme.accentSoft : (btn.hovered ? Theme.sunk : "transparent")
                            border.width: btn.visualFocus ? 2 : 0
                            border.color: Theme.accent
                        }
                        contentItem: RowLayout {
                            spacing: 8
                            Item { Layout.preferredWidth: 2 }
                            Icon {
                                path: modelData.icon || ""
                                color: modelData.id === root.current ? Theme.accent : Theme.muted
                            }
                            SbText {
                                text: btn.text
                                font.weight: Font.Medium
                                color: modelData.id === root.current ? Theme.ink : Theme.muted
                            }
                            Item { Layout.preferredWidth: 6 }
                        }
                    }
                }
            }
        }
    }
}
