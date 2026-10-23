// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import SynBench

// Renders a test's DetailLayout, already turned into plain data (P6): one
// renderer for every test. Phase 4a D24/D25/D28: the top blocks, then every
// chart full width, then the tables, collapsed until opened; which tables are
// open is remembered for the session in DetailState.
ColumnLayout {
    id: root

    property var blocks: []
    property string colorToken
    property string testId

    // Phase 4a D24: every page is the top blocks, then the charts, then the
    // tables. A neutral note explains the block before it (§22.3); any other
    // note is about the run and goes to the top.
    function arrange(list) {
        const top = [], charts = [], tables = []
        const owner = new Map()  // block index -> its chart/table entry
        for (let i = 0; i < list.length; ++i) {
            const b = list[i]
            if (b.type === "linechart") {
                const e = { block: b, notes: [] }; charts.push(e); owner.set(i, e)
            } else if (b.type === "table") {
                const e = { block: b, notes: [] }; tables.push(e); owner.set(i, e)
            } else if (b.type === "note" && b.tone === "neutral") {
                let j = i - 1
                while (j >= 0 && list[j].type === "note") --j
                if (j < 0 || !owner.has(j)) top.push(b)
                else if (list[j].type === "table" && j > 0 && list[j - 1].type === "linechart") owner.get(j - 1).notes.push(b)
                else owner.get(j).notes.push(b)
            } else {
                top.push(b)
            }
        }
        return { top: top, charts: charts, tables: tables }
    }
    readonly property var sections: arrange(blocks)
    function tableKey(index) {
        const title = sections.tables[index].block.title
        let n = 0
        for (let i = 0; i < index; ++i) if (sections.tables[i].block.title === title) ++n
        return testId + "|" + title + "|" + n
    }
    readonly property var tableKeys: sections.tables.map((e, i) => tableKey(i))
    readonly property bool allExpanded: DetailState.revision >= 0 && tableKeys.length > 0
                                        && tableKeys.every(k => DetailState.isExpanded(k))

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

    // The top: KPIs, grids, N/A, and every note about the run.
    Repeater {
        model: root.sections.top
        delegate: Loader {
            id: head
            required property var modelData
            Layout.fillWidth: true
            sourceComponent: root.component(modelData.type)
            // The loaded block's data (and a chart's colour) stay bound.
            Binding {
                target: head.item
                property: "block"
                value: head.modelData.type === "custom"
                    ? { text: qsTr("This view isn't available in this version."), tone: "warn" } : head.modelData
            }
            Binding {
                when: head.item !== null && head.item.hasOwnProperty("colorToken")
                target: head.item
                property: "colorToken"
                value: root.colorToken
            }
        }
    }

    // The charts, one per row (D28), each with the notes that explain it.
    Repeater {
        model: root.sections.charts
        delegate: ColumnLayout {
            id: chartEntry
            required property var modelData
            Layout.fillWidth: true
            spacing: 8
            Loader {
                id: chartLoader
                Layout.fillWidth: true
                sourceComponent: chart
                Binding { target: chartLoader.item; property: "block"; value: chartEntry.modelData.block }
                Binding {
                    when: chartLoader.item !== null && chartLoader.item.hasOwnProperty("colorToken")
                    target: chartLoader.item
                    property: "colorToken"
                    value: root.colorToken
                }
            }
            Repeater {
                model: chartEntry.modelData.notes
                delegate: NoteBlock {
                    required property var modelData
                    objectName: "chartNote"
                    Layout.fillWidth: true
                    block: modelData
                }
            }
        }
    }

    RowLayout {
        objectName: "tablesBar"
        visible: root.sections.tables.length > 0
        Layout.fillWidth: true
        SbText { text: qsTr("Tables"); kind: "label"; Layout.fillWidth: true }
        SbButton {
            objectName: "expandAll"
            text: root.allExpanded ? qsTr("Collapse all") : qsTr("Expand all")
            onClicked: DetailState.setAll(root.tableKeys, !root.allExpanded)
        }
    }

    // The tables, collapsed until opened (D25); a table's notes show only
    // while it is open.
    Repeater {
        model: root.sections.tables
        delegate: ColumnLayout {
            id: tableEntry
            required property var modelData
            required property int index
            Layout.fillWidth: true
            spacing: 8
            TableBlock {
                id: tableItem
                objectName: "detailTable"
                Layout.fillWidth: true
                collapsible: true
                block: tableEntry.modelData.block
                expanded: DetailState.revision >= 0 && DetailState.isExpanded(root.tableKeys[tableEntry.index])
                onExpandRequested: (on) => DetailState.setExpanded(root.tableKeys[tableEntry.index], on)
            }
            Repeater {
                model: tableItem.expanded ? tableEntry.modelData.notes : []
                delegate: NoteBlock {
                    required property var modelData
                    objectName: "tableNote"
                    Layout.fillWidth: true
                    block: modelData
                }
            }
        }
    }
}
