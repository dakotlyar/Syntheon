// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import SynBench

// A table of already-formatted cells; the first column left, the rest right in
// the data font. Columns are sized to their content (the widest header or
// cell line, a cell may hold several lines): the value columns get what they
// need, the first column the rest. When even that doesn't fit (a narrow
// window), the headers wrap at their spaces first, then the first column gives
// way (down to 30%), then every column shrinks in proportion and the cells wrap.
Box {
    id: root

    property var block
    readonly property var columns: block ? block.columns : []
    readonly property var rows: block ? block.rows : []
    readonly property var boldRows: block && block.boldRows ? block.boldRows : []

    implicitHeight: content.implicitHeight
    clip: true

    readonly property int gap: 12
    readonly property real available: Math.max(0, width - 24 - gap * Math.max(0, columns.length - 1))

    // Fonts the cells use, for measuring.
    SbText { id: headProbe; visible: false; kind: "label"; font.capitalization: Font.MixedCase }
    SbText { id: bodyProbe; visible: false; kind: "body"; font.pixelSize: 13 }
    SbText { id: dataProbe; visible: false; kind: "data"; font.pixelSize: 13 }
    FontMetrics { id: headMetrics; font: headProbe.font }
    FontMetrics { id: bodyMetrics; font: bodyProbe.font }
    FontMetrics { id: dataMetrics; font: dataProbe.font }

    function lineWidth(metrics, text, spacing) {
        let w = 0
        for (const line of String(text).split("\n"))
            w = Math.max(w, metrics.advanceWidth(line) + spacing * line.length)
        return Math.ceil(w) + 2
    }
    // The widest word of a header (what it needs when it may wrap).
    function wordWidth(metrics, text, spacing) {
        let w = 0
        for (const word of String(text).split(/[ \n]/))
            w = Math.max(w, metrics.advanceWidth(word) + spacing * word.length)
        return Math.ceil(w) + 2
    }
    // What each column needs when its headers wrap: their widest word, and
    // the widest cell line.
    readonly property var compactNeeds: {
        const n = columns.length
        const out = []
        for (let c = 0; c < n; ++c)
            out.push(wordWidth(headMetrics, headerText(columns[c]), headProbe.font.letterSpacing))
        for (const row of rows) {
            for (let c = 0; c < Math.min(n, row.length); ++c)
                out[c] = Math.max(out[c], lineWidth(c === 0 ? bodyMetrics : dataMetrics, row[c], 0))
        }
        return out
    }
    // What each column needs so no word is broken: its widest header word
    // and widest cell word.
    readonly property var wordNeeds: {
        const n = columns.length
        const out = []
        for (let c = 0; c < n; ++c)
            out.push(wordWidth(headMetrics, headerText(columns[c]), headProbe.font.letterSpacing))
        for (const row of rows) {
            for (let c = 0; c < Math.min(n, row.length); ++c)
                out[c] = Math.max(out[c], wordWidth(c === 0 ? bodyMetrics : dataMetrics, row[c], 0))
        }
        return out
    }
    // What each column needs to show its widest header or cell line.
    readonly property var needs: {
        const n = columns.length
        const out = []
        for (let c = 0; c < n; ++c)
            out.push(lineWidth(headMetrics, headerText(columns[c]), headProbe.font.letterSpacing))
        for (const row of rows) {
            for (let c = 0; c < Math.min(n, row.length); ++c)
                out[c] = Math.max(out[c], lineWidth(c === 0 ? bodyMetrics : dataMetrics, row[c], 0))
        }
        return out
    }
    readonly property var widths: {
        let need = needs
        const n = need.length
        if (n === 0)
            return []
        let total = need.reduce((a, b) => a + b, 0)
        if (total <= available) {
            const w = need.slice()
            w[0] += available - total  // the first column takes the spare room
            return w
        }
        // Too narrow for one-line headers: they wrap (at a space) before any
        // cell does.
        need = compactNeeds
        total = need.reduce((a, b) => a + b, 0)
        if (total <= available) {
            const w = need.slice()
            w[0] += available - total
            return w
        }
        const rest = total - need[0]
        // ...down to 30%, but never below its widest word (AP-5 L6).
        const first = Math.max(wordNeeds[0], Math.min(need[0], available * 0.3))
        if (first + rest <= available)
            return [available - rest].concat(need.slice(1))
        // Every column gets its widest word, and the room left is shared in
        // proportion to what each still wants, so cells wrap between words
        // (Zeb D1: "G.Skil|l", "TYP|E" at 760 px)...
        const words = wordNeeds
        const wordTotal = words.reduce((a, b) => a + b, 0)
        if (wordTotal <= available) {
            const want = need.map((x, i) => Math.max(0, x - words[i]))
            const wantTotal = want.reduce((a, b) => a + b, 0)
            const spare = available - wordTotal
            return words.map((x, i) => x + (wantTotal > 0 ? want[i] * spare / wantTotal : spare / n))
        }
        // ...and only when even that doesn't fit, everything shrinks and
        // words break.
        return need.map(x => x * available / total)
    }
    function columnWidth(i) { return widths.length > i ? widths[i] : 0 }

    readonly property var unitHeaders: ["ns", "µs", "ms", "s", "min", "fps", "GB/s", "MB/s", "IOPS", "MT/s", "Gpx/s",
                                        "Mpx/s", "MOps/s", "MFLOPS", "tok/s", "inf/s", "it/s", "W", "Wh", "%", "points"]
    // A header that is just a unit keeps its case; anything else is capitals.
    function headerText(s) { return unitHeaders.indexOf(s) >= 0 ? s : s.toUpperCase() }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        SbText {
            visible: root.block && root.block.title.length > 0
            Layout.margins: 12
            text: root.block ? root.block.title : ""
            font.weight: Font.DemiBold
        }

        // header
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: headRow.implicitHeight + 16
            color: Theme.sunk
            Row {
                id: headRow
                anchors.left: parent.left; anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 12; anchors.rightMargin: 12
                spacing: root.gap
                Repeater {
                    model: root.columns
                    delegate: SbText {
                        required property string modelData
                        required property int index
                        width: root.columnWidth(index)
                        horizontalAlignment: index === 0 ? Text.AlignLeft : Text.AlignRight
                        // Capitals by hand: automatic capitalisation would turn
                        // "µs" into a Greek capital Mu and "GB/s" into "GB/S".
                        text: root.headerText(modelData)
                        kind: "label"
                        font.capitalization: Font.MixedCase
                        // At a word, else anywhere: a word wider than its
                        // column wraps instead of running into the next one.
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
        Repeater {
            model: root.rows
            delegate: Item {
                id: tableRow
                required property var modelData
                required property int index
                Layout.fillWidth: true
                implicitHeight: rowLayout.implicitHeight + 16
                Rectangle {
                    visible: tableRow.index < root.rows.length - 1
                    anchors.bottom: parent.bottom
                    width: parent.width; height: 1; color: Theme.line
                }
                Row {
                    id: rowLayout
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 12; anchors.rightMargin: 12
                    spacing: root.gap
                    Repeater {
                        model: tableRow.modelData
                        delegate: SbText {
                            required property string modelData
                            required property int index
                            width: root.columnWidth(index)
                            horizontalAlignment: index === 0 ? Text.AlignLeft : Text.AlignRight
                            text: modelData
                            kind: index === 0 ? "body" : "data"
                            font.pixelSize: 13
                            font.weight: root.boldRows.indexOf(tableRow.index) >= 0 ? Font.DemiBold : Font.Normal
                            // At a word, else anywhere (Zeb D1: a part number
                            // ran into the next column at 760 px).
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }
    }
}
