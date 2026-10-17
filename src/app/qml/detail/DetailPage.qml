// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import SynBench

// Renders a test's DetailLayout, already turned into plain data (P6): one
// renderer for every test. Charts sit beside the tables that follow them, and
// consecutive tables pair up, when there is room (as in the mockup).
ColumnLayout {
    id: root

    property var blocks: []
    property string colorToken
    readonly property bool wide: width >= 760

    // Groups the blocks into rows: [{kind: "single"|"split"|"pair", blocks: [...]}].
    function group(list) {
        const rows = []
        let i = 0
        while (i < list.length) {
            const b = list[i]
            if (b.type === "linechart") {
                const tail = []
                let j = i + 1
                while (j < list.length && list[j].type === "table" && tail.length < 3) tail.push(list[j++])
                rows.push(tail.length > 0 ? { kind: "split", blocks: [b].concat(tail) } : { kind: "single", blocks: [b] })
                i = j
            } else if (b.type === "table" && i + 1 < list.length && list[i + 1].type === "table") {
                rows.push({ kind: "pair", blocks: [b, list[i + 1]] })
                i += 2
            } else {
                rows.push({ kind: "single", blocks: [b] })
                i += 1
            }
        }
        return rows
    }
    readonly property var rows: group(blocks)

    spacing: 16

    Component { id: kpis; KpisBlock { } }
    Component { id: table; TableBlock { } }
    Component { id: grid; GridBlock { } }
    Component { id: chart; ChartBlock { } }
    Component { id: note; NoteBlock { } }
    Component { id: na; NaStateBlock { } }
    Component {
        id: unknown
        NoteBlock { }  // a custom block this build can't draw
    }

    function component(type) {
        switch (type) {
        case "kpis": return kpis
        case "table": return table
        case "grid": return grid
        case "linechart": return chart
        case "note": return note
        case "nastate": return na
        default: return unknown
        }
    }

    Repeater {
        model: root.rows
        delegate: Item {
            id: rowItem
            required property var modelData
            Layout.fillWidth: true
            implicitHeight: rowGrid.implicitHeight

            GridLayout {
                id: rowGrid
                anchors.left: parent.left
                anchors.right: parent.right
                columns: rowItem.modelData.kind === "single" || !root.wide ? 1 : 2
                columnSpacing: 16
                rowSpacing: 16

                // The first block of a row (the chart, or a lone block).
                Loader {
                    id: head
                    Layout.fillWidth: true
                    Layout.preferredWidth: rowItem.modelData.kind === "split" ? 3 : 1
                    Layout.alignment: Qt.AlignTop
                    sourceComponent: root.component(rowItem.modelData.blocks[0].type)
                    // The loaded block's data (and a chart's colour) stay bound.
                    Binding {
                        target: head.item
                        property: "block"
                        value: rowItem.modelData.blocks[0].type === "custom"
                            ? { text: qsTr("This view isn't available in this version."), tone: "warn" } : rowItem.modelData.blocks[0]
                    }
                    Binding {
                        when: head.item !== null && head.item.hasOwnProperty("colorToken")
                        target: head.item
                        property: "colorToken"
                        value: root.colorToken
                    }
                }
                // The rest of the row, stacked in the second column.
                ColumnLayout {
                    visible: rowItem.modelData.blocks.length > 1
                    Layout.fillWidth: true
                    Layout.preferredWidth: rowItem.modelData.kind === "split" ? 2 : 1
                    Layout.alignment: Qt.AlignTop
                    spacing: 16
                    Repeater {
                        model: rowItem.modelData.blocks.slice(1)
                        delegate: Loader {
                            id: tail
                            required property var modelData
                            Layout.fillWidth: true
                            sourceComponent: root.component(modelData.type)
                            Binding { target: tail.item; property: "block"; value: tail.modelData }
                        }
                    }
                }
            }
        }
    }
}
