// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import SynBench

// The mockup's select. `items` = [{value, label, enabled?, disabledReason?, detail?}];
// the chosen item's value is `current`.
ComboBox {
    id: root

    property var items: []
    // The list can be wider than the closed combo (long labels aren't cut there).
    property real popupMinimumWidth: 0
    property string current
    signal chosen(string value)

    model: items
    textRole: "label"
    valueRole: "value"
    currentIndex: {
        for (var i = 0; i < items.length; ++i)
            if (items[i].value === current) return i
        return -1
    }
    onActivated: index => root.chosen(items[index].value)
    implicitHeight: 34
    hoverEnabled: true
    Accessible.name: displayText

    background: Rectangle {
        radius: Theme.radius
        color: Theme.sunk
        border.width: root.visualFocus ? 2 : 1
        border.color: root.visualFocus || root.hovered ? Theme.accent : Theme.line
    }
    contentItem: SbText {
        leftPadding: 9
        rightPadding: 28
        text: root.displayText
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        font.pixelSize: 13
    }
    indicator: Icon {
        x: root.width - width - 10
        y: (root.height - height) / 2
        width: 10; height: 10
        path: "M2 5l6 6 6-6"
        color: Theme.muted
    }
    delegate: ItemDelegate {
        id: row
        required property var modelData
        required property int index
        width: root.popup.width
        height: 38
        enabled: modelData.enabled !== false
        highlighted: root.highlightedIndex === index
        contentItem: Column {
            spacing: 0
            SbText {
                text: row.modelData.label
                width: row.width - 20
                elide: Text.ElideRight
                font.pixelSize: 13
                color: row.enabled ? Theme.ink : Theme.na
            }
            SbText {
                visible: text.length > 0
                text: row.modelData.disabledReason || row.modelData.detail || ""
                width: row.width - 20
                elide: Text.ElideRight
                kind: "small"
                font.pixelSize: 11
            }
        }
        background: Rectangle { color: row.highlighted ? Theme.accentSoft : Theme.panel }
    }
    popup: Popup {
        y: root.height + 2
        width: Math.max(root.width, root.popupMinimumWidth)
        implicitHeight: Math.min(contentItem.implicitHeight + 2, 280)
        padding: 1
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: Theme.radius }
    }
}
