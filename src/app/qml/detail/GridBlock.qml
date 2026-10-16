// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import SynBench

// The CrystalDiskMark-style grid (Storage): row labels down the side, a
// value with its unit (and IOPS) in each cell. One flat grid so the header
// and the cells line up.
Box {
    id: root

    property var block
    readonly property var columns: block ? block.columns : []
    readonly property var rows: block ? block.rows : []

    // [{kind: "head"|"label"|"cell", ...}] in row-major order.
    readonly property var flat: {
        const out = [{ kind: "head", text: "" }]
        for (const c of columns) out.push({ kind: "head", text: c })
        for (const r of rows) {
            out.push({ kind: "label", label: r.label, small: r.small })
            for (const cell of r.cells) out.push({ kind: "cell", main: cell.main, sub: cell.sub })
        }
        return out
    }

    implicitHeight: grid.implicitHeight + 24

    GridLayout {
        id: grid
        anchors.fill: parent
        anchors.margins: 12
        columns: 1 + root.columns.length
        columnSpacing: 6
        rowSpacing: 6

        Repeater {
            model: root.flat
            delegate: Rectangle {
                id: cellItem
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.fillHeight: modelData.kind !== "head"
                implicitHeight: modelData.kind === "head" ? headText.implicitHeight + 8 : body.implicitHeight + 20
                radius: 6
                color: modelData.kind === "label" ? Theme.accentSoft : modelData.kind === "cell" ? Theme.sunk : "transparent"

                SbText {
                    id: headText
                    visible: cellItem.modelData.kind === "head"
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    verticalAlignment: Text.AlignVCenter
                    text: cellItem.modelData.text || ""
                    kind: "label"
                }
                ColumnLayout {
                    id: body
                    visible: cellItem.modelData.kind !== "head"
                    anchors.fill: parent
                    anchors.margins: 10
                    anchors.leftMargin: cellItem.modelData.kind === "label" ? 12 : 10
                    spacing: 0
                    SbText {
                        objectName: cellItem.modelData.kind === "cell" ? "gridMain" : "gridLabel"
                        Layout.fillWidth: true
                        horizontalAlignment: cellItem.modelData.kind === "cell" ? Text.AlignRight : Text.AlignLeft
                        text: cellItem.modelData.kind === "cell" ? cellItem.modelData.main : (cellItem.modelData.label || "")
                        kind: cellItem.modelData.kind === "cell" ? "data" : "body"
                        font.pixelSize: cellItem.modelData.kind === "cell" ? 16 : 13
                        font.weight: cellItem.modelData.kind === "label" ? Font.DemiBold : Font.Normal
                    }
                    SbText {
                        Layout.fillWidth: true
                        horizontalAlignment: cellItem.modelData.kind === "cell" ? Text.AlignRight : Text.AlignLeft
                        text: cellItem.modelData.kind === "cell" ? cellItem.modelData.sub : (cellItem.modelData.small || "")
                        kind: "small"
                        font.pixelSize: 11
                    }
                }
            }
        }
    }
}
