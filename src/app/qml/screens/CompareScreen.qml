// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// N runs, one column each, % change against a baseline the user picks
// (default: the newest run) (D5). A "scoring changed" marker on a row whose
// test or formula version differs between the runs, raw metrics still
// compared (D6). A Stopped or Interrupted run's overall carries a "partial" marker and no change % (N2).
ScreenFrame {
    id: root

    title: qsTr("Compare runs")
    subtitle: Compare.subtitle

    actions: [
        SbButton {
            objectName: "changeSelection"
            text: qsTr("Change selection")
            onClicked: AppController.section = "history"
        }
    ]

    readonly property var runs: Compare.runs
    readonly property int n: runs.length

    function runLabel(r) { return r.date + (r.machine.length > 0 ? ", " + r.machine : "") }

    Box {
        objectName: "emptyState"
        visible: !Compare.valid
        Layout.fillWidth: true
        implicitHeight: emptyCol.implicitHeight + 36
        ColumnLayout {
            id: emptyCol
            anchors.fill: parent
            anchors.margins: 18
            spacing: 10
            SbText { text: qsTr("Nothing to compare yet"); font.weight: Font.DemiBold }
            SbText {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Tick two or more runs in History, then press Compare.")
                kind: "small"
            }
            SbButton { text: qsTr("Go to History"); onClicked: AppController.section = "history" }
        }
    }

    // --- baseline picker ---
    RowLayout {
        visible: Compare.valid
        Layout.fillWidth: true
        spacing: 10
        SbText { text: qsTr("Compare against"); kind: "label" }
        SbCombo {
            objectName: "baselinePicker"
            Layout.preferredWidth: Math.min(460, root.width - 180)
            popupMinimumWidth: 520
            items: root.runs.map(function(r, i) {
                return { value: r.uuid, label: root.runLabel(r) + (i === 0 ? qsTr(" (newest)") : ""), detail: "" }
            })
            current: Compare.baselineUuid
            onChosen: value => Compare.setBaseline(value)
        }
    }

    // --- scores ---
    Box {
        id: scoreTable
        objectName: "scoreTable"
        visible: Compare.valid
        Layout.fillWidth: true
        implicitHeight: scoreGrid.implicitHeight

        readonly property var flat: {
            const out = [{ kind: "head", text: qsTr("Test") }]
            for (const r of root.runs)
                out.push({ kind: "runhead", date: r.date, machine: r.machine, day: r.day, timeAndHost: r.timeAndHost, status: r.status, baseline: r.baseline })
            out.push({ kind: "head", text: qsTr("Relative") })
            for (const row of Compare.scoreRows) {
                out.push({ kind: "name", name: row.name, colorToken: row.colorToken, scoringChanged: row.scoringChanged, overall: row.overall, testId: row.testId })
                for (const c of row.cells) out.push({ kind: "score", cell: c, overall: row.overall })
                out.push({ kind: "bars", bars: row.bars })
            }
            return out
        }

        GridLayout {
            id: scoreGrid
            anchors.left: parent.left
            anchors.right: parent.right
            columns: root.n + 2
            columnSpacing: 0
            rowSpacing: 0

            Repeater {
                model: scoreTable.flat
                delegate: Item {
                    id: cell
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredWidth: cell.modelData.kind === "name" || cell.modelData.kind === "head" && cell.modelData.text === qsTr("Test") ? 2 : 1
                    implicitHeight: cell.modelData.kind === "runhead" ? 56 : cell.modelData.kind === "head" ? 34 : 52
                    clip: true

                    Rectangle {
                        anchors.fill: parent
                        color: (cell.modelData.kind === "head" || cell.modelData.kind === "runhead") ? Theme.sunk : "transparent"
                    }
                    Rectangle {
                        visible: cell.modelData.kind !== "head" && cell.modelData.kind !== "runhead"
                        anchors.bottom: parent.bottom
                        width: parent.width; height: 1; color: Theme.line
                    }

                    SbText {
                        visible: cell.modelData.kind === "head"
                        anchors.fill: parent
                        anchors.leftMargin: 14; anchors.rightMargin: 14
                        verticalAlignment: Text.AlignVCenter
                        text: cell.modelData.text || ""
                        kind: "label"
                    }
                    ColumnLayout {
                        visible: cell.modelData.kind === "runhead"
                        anchors.fill: parent
                        anchors.leftMargin: 14; anchors.rightMargin: 14
                        spacing: 0
                        Item { Layout.fillHeight: true }
                        SbText { objectName: "runDay"; text: cell.modelData.day || ""; kind: "small"; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight }
                        SbText { objectName: "runTime"; text: cell.modelData.timeAndHost || ""; kind: "small"; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight }
                        // Every run header reserves the marker's line, so the date and
                        // time sit at the same height in all columns; only the baseline
                        // column shows it.
                        Pill {
                            objectName: "baselineMarker"
                            Layout.alignment: Qt.AlignRight
                            visible: cell.modelData.kind === "runhead"
                            opacity: cell.modelData.baseline === true ? 1 : 0
                            text: qsTr("baseline")
                            tone: "acc"
                            Accessible.ignored: cell.modelData.baseline !== true
                        }
                        Item { Layout.fillHeight: true }
                    }
                    // The full date and machine on hover, whatever the column width.
                    HoverHandler { id: headHover; enabled: cell.modelData.kind === "runhead" }
                    ToolTip.visible: headHover.hovered && cell.modelData.kind === "runhead"
                    ToolTip.text: cell.modelData.kind === "runhead" ? cell.modelData.date + "\n" + cell.modelData.machine : ""
                    RowLayout {
                        visible: cell.modelData.kind === "name"
                        anchors.fill: parent
                        anchors.leftMargin: 14; anchors.rightMargin: 8
                        spacing: 10
                        Swatch { implicitHeight: 26; color: cell.modelData.overall ? Theme.ink : Theme.testColor(cell.modelData.colorToken || "") }
                        SbText { text: cell.modelData.name || ""; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true }
                        Pill {
                            objectName: "scoringChanged"
                            visible: cell.modelData.scoringChanged === true
                            text: qsTr("scoring changed")
                            tone: "warn"
                        }
                    }
                    ColumnLayout {
                        visible: cell.modelData.kind === "score"
                        anchors.fill: parent
                        anchors.leftMargin: 8; anchors.rightMargin: 14
                        spacing: 0
                        Item { Layout.fillHeight: true }
                        RowLayout {
                            Layout.alignment: Qt.AlignRight
                            spacing: 6
                            Pill {
                                objectName: "partial"
                                visible: cell.modelData.kind === "score" && cell.modelData.cell.partial === true
                                text: qsTr("partial")
                                tone: "warn"
                            }
                            SbText {
                                objectName: "scoreText"
                                text: cell.modelData.kind === "score" ? cell.modelData.cell.text : ""
                                kind: "data"
                                font.pixelSize: 15
                                color: cell.modelData.kind === "score" && cell.modelData.cell.na ? Theme.na : Theme.ink
                            }
                        }
                        SbText {
                            objectName: "changeText"
                            Layout.alignment: Qt.AlignRight
                            visible: text.length > 0
                            text: cell.modelData.kind === "score" ? cell.modelData.cell.changeText : ""
                            kind: "data"
                            font.pixelSize: 12
                            color: cell.modelData.kind === "score"
                                   ? (cell.modelData.cell.direction === "good" ? Theme.good : cell.modelData.cell.direction === "bad" ? Theme.bad : Theme.muted)
                                   : Theme.muted
                        }
                        Item { Layout.fillHeight: true }
                    }
                    // The relative bars: one per run, the baseline in the accent colour.
                    Column {
                        id: barColumn
                        visible: cell.modelData.kind === "bars"
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 14; anchors.rightMargin: 14
                        spacing: 3
                        Repeater {
                            model: cell.modelData.kind === "bars" ? cell.modelData.bars : []
                            delegate: Rectangle {
                                required property real modelData
                                required property int index
                                height: 6; radius: 3
                                width: Math.max(2, barColumn.width * modelData)
                                color: root.runs[index] && root.runs[index].baseline ? Theme.accent : Theme.muted
                                opacity: root.runs[index] && root.runs[index].baseline ? 1 : 0.55
                            }
                        }
                    }
                }
            }
        }
    }

    // --- raw metrics for one test ---
    Box {
        objectName: "rawTable"
        visible: Compare.valid && Compare.rawTests.length > 0
        Layout.fillWidth: true
        implicitHeight: rawColumn.implicitHeight

        ColumnLayout {
            id: rawColumn
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 12
                SbText { text: qsTr("Raw metrics, side by side"); font.weight: Font.DemiBold; Layout.fillWidth: true }
                SbText { text: qsTr("Showing"); kind: "small" }
                SbCombo {
                    objectName: "rawTestPicker"
                    Layout.preferredWidth: 180
                    items: Compare.rawTests
                    current: Compare.rawTestId
                    onChosen: value => Compare.setRawTest(value)
                }
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }

            GridLayout {
                id: rawGrid
                Layout.fillWidth: true
                columns: 1 + root.n
                columnSpacing: 0
                rowSpacing: 0
                Repeater {
                    model: {
                        const out = [{ kind: "head", text: qsTr("Metric") }]
                        for (const r of root.runs) out.push({ kind: "head", text: r.day + "\n" + r.timeAndHost, tip: r.date + "\n" + r.machine })
                        for (const row of Compare.rawRows) {
                            out.push({ kind: "label", text: row.label })
                            for (const c of row.cells) out.push({ kind: "value", cell: c })
                        }
                        return out
                    }
                    delegate: Item {
                        id: rcell
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: rcell.modelData.kind === "label" || rcell.modelData.text === qsTr("Metric") ? 2 : 1
                        implicitHeight: rcell.modelData.kind === "head" ? 44 : 46
                        HoverHandler { id: rawHover; enabled: rcell.modelData.tip !== undefined }
                        ToolTip.visible: rawHover.hovered && rcell.modelData.tip !== undefined
                        ToolTip.text: rcell.modelData.tip || ""
                        clip: true
                        Rectangle { anchors.fill: parent; color: rcell.modelData.kind === "head" ? Theme.sunk : "transparent" }
                        Rectangle {
                            visible: rcell.modelData.kind !== "head"
                            anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line
                        }
                        SbText {
                            visible: rcell.modelData.kind === "head" || rcell.modelData.kind === "label"
                            anchors.fill: parent
                            anchors.leftMargin: 14; anchors.rightMargin: 14
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: rcell.modelData.kind === "head" && rcell.modelData.text !== qsTr("Metric") ? Text.AlignRight : Text.AlignLeft
                            text: rcell.modelData.text || ""
                            kind: rcell.modelData.kind === "head" ? "label" : "body"
                            elide: Text.ElideRight
                        }
                        ColumnLayout {
                            visible: rcell.modelData.kind === "value"
                            anchors.fill: parent
                            anchors.rightMargin: 14
                            spacing: 0
                            Item { Layout.fillHeight: true }
                            SbText {
                                objectName: "rawValue"
                                Layout.alignment: Qt.AlignRight
                                text: rcell.modelData.kind === "value" ? rcell.modelData.cell.text : ""
                                kind: "data"; font.pixelSize: 13
                            }
                            SbText {
                                objectName: "rawChange"
                                Layout.alignment: Qt.AlignRight
                                visible: text.length > 0
                                text: rcell.modelData.kind === "value" ? rcell.modelData.cell.changeText : ""
                                kind: "data"; font.pixelSize: 11
                                color: rcell.modelData.kind === "value"
                                       ? (rcell.modelData.cell.direction === "good" ? Theme.good : rcell.modelData.cell.direction === "bad" ? Theme.bad : Theme.muted)
                                       : Theme.muted
                            }
                            Item { Layout.fillHeight: true }
                        }
                    }
                }
            }
        }
    }
}
