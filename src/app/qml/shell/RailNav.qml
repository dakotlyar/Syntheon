// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// The left rail of sections (the default layout).
Rectangle {
    id: root

    property var sections: []
    property string current
    property string version
    signal navigate(string id)

    implicitWidth: 176
    color: Theme.win

    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.line }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        anchors.topMargin: 12
        spacing: 2

        Repeater {
            model: root.sections
            delegate: Item {
                required property var modelData
                Layout.fillWidth: true
                Layout.fillHeight: modelData.spacer === true
                Layout.preferredHeight: modelData.spacer === true ? 0 : 36
                visible: true

                AbstractButton {
                    id: btn
                    objectName: "nav-" + (modelData.id || "")
                    visible: !modelData.spacer
                    anchors.fill: parent
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
                        spacing: 10
                        Item { Layout.preferredWidth: 4 }
                        Icon {
                            path: modelData.icon || ""
                            color: modelData.id === root.current ? Theme.accent : Theme.muted
                        }
                        SbText {
                            Layout.fillWidth: true
                            text: btn.text
                            font.weight: Font.Medium
                            color: modelData.id === root.current ? Theme.ink : Theme.muted
                        }
                    }
                }
            }
        }
        SbText {
            Layout.leftMargin: 10
            text: root.version
            kind: "data"
            font.pixelSize: 11
            color: Theme.muted
            elide: Text.ElideRight
            Layout.maximumWidth: parent.width - 20
        }
    }
}
