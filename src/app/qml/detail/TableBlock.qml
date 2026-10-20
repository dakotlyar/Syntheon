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
    readonly property alias tokenMetrics: dataMetrics  // for tests
    SbText { id: bodyBoldProbe; visible: false; kind: "body"; font.pixelSize: 13; font.weight: Font.DemiBold }
    SbText { id: dataBoldProbe; visible: false; kind: "data"; font.pixelSize: 13; font.weight: Font.DemiBold }
    FontMetrics { id: bodyBoldMetrics; font: bodyBoldProbe.font }
    FontMetrics { id: dataBoldMetrics; font: dataBoldProbe.font }

    function lineWidth(metrics, text, spacing) {
        let w = 0
        for (const line of String(text).split("\n"))
            w = Math.max(w, metrics.advanceWidth(line) + spacing * line.length)
        return Math.ceil(w) + 2
    }
    // Cells are wrapped here, not by Qt (Zeb C1, Defect B): Qt's line
    // breaking also breaks after "/" and "-" ("GB/|s", "SUB-|SCORE"), and
    // stopping it with an invisible joiner needed a glyph no bundled font
    // has. A cell's tokens are its space-separated words; only a long token
    // (an identifier like "Controller0-ChannelA-DIMM0") may also break after
    // its hyphens (N1); underscores never (N2). Both the column sizing and
    // the wrapping use these tokens (AP-5 L6).
    readonly property int longToken: 16
    function tokens(line) {
        const out = []
        for (const word of String(line).split(" ")) {
            // Split after a hyphen before a letter ("Controller0-ChannelA"),
            // not before a digit ("F5-6000J3038F16G" stays whole).
            const pieces = []
            let from = 0
            if (word.length >= longToken) {
                for (let i = 1; i < word.length - 1; ++i) {
                    if (word[i] === "-" && /[A-Za-z]/.test(word[i + 1])) {
                        pieces.push(word.slice(from, i))
                        from = i + 1
                    }
                }
            }
            pieces.push(word.slice(from))
            for (let i = 0; i < pieces.length; ++i) {
                const last = i === pieces.length - 1
                // "Controller0-" "ChannelA-" "DIMM0": hyphen pieces join with
                // nothing, words with a space.
                out.push({ text: last ? pieces[i] : pieces[i] + "-", glue: last ? " " : "" })
            }
        }
        return out
    }
    function measure(metrics, text, spacing) {
        return metrics.advanceWidth(text) + spacing * text.length
    }
    function wordWidth(metrics, text, spacing) {
        let w = 0
        for (const line of String(text).split("\n"))
            for (const t of tokens(line))
                w = Math.max(w, measure(metrics, t.text, spacing))
        return Math.ceil(w) + 2
    }
    // A cell's text for `width`: its own line breaks kept, each line filled
    // greedily with whole tokens. A token wider than the column on its own (a
    // part number at 760 px, Zeb D1) is split by characters; only that token
    // (AP-5 round 4 M1: not "G.Sk|ill" beside it).
    function wrapped(text, width, metrics, spacing) {
        const lines = []
        for (const line of String(text).split("\n")) {
            let current = ""
            let glue = ""
            for (const t of tokens(line)) {
                if (measure(metrics, t.text, spacing) > width + 1) {
                    if (current.length > 0)
                        lines.push(current)
                    current = ""
                    for (const ch of t.text) {
                        if (current.length > 0 && measure(metrics, current + ch, spacing) > width + 1) {
                            lines.push(current)
                            current = ""
                        }
                        current += ch
                    }
                    glue = t.glue
                    continue
                }
                const candidate = current.length === 0 ? t.text : current + glue + t.text
                if (current.length > 0 && measure(metrics, candidate, spacing) > width + 1) {
                    lines.push(current)
                    current = t.text
                } else {
                    current = candidate
                }
                glue = t.glue
            }
            lines.push(current)
        }
        return lines.join("\n")
    }
    // The metrics a cell is drawn with: body (first column) or data, DemiBold
    // in a bold row (AP-5 round 4 M2: sized as drawn).
    function cellMetrics(row, c) {
        const bold = boldRows.indexOf(row) >= 0
        return c === 0 ? (bold ? bodyBoldMetrics : bodyMetrics) : (bold ? dataBoldMetrics : dataMetrics)
    }
    // What each column needs when its headers wrap: their widest word, and
    // the widest cell line.
    readonly property var compactNeeds: {
        const n = columns.length
        const out = []
        for (let c = 0; c < n; ++c)
            out.push(wordWidth(headMetrics, headerText(columns[c]), headProbe.font.letterSpacing))
        for (let r = 0; r < rows.length; ++r) {
            for (let c = 0; c < Math.min(n, rows[r].length); ++c)
                out[c] = Math.max(out[c], lineWidth(cellMetrics(r, c), rows[r][c], 0))
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
        for (let r = 0; r < rows.length; ++r) {
            for (let c = 0; c < Math.min(n, rows[r].length); ++c)
                out[c] = Math.max(out[c], wordWidth(cellMetrics(r, c), rows[r][c], 0))
        }
        return out
    }
    // What each column needs to show its widest header or cell line.
    readonly property var needs: {
        const n = columns.length
        const out = []
        for (let c = 0; c < n; ++c)
            out.push(lineWidth(headMetrics, headerText(columns[c]), headProbe.font.letterSpacing))
        for (let r = 0; r < rows.length; ++r) {
            for (let c = 0; c < Math.min(n, rows[r].length); ++c)
                out[c] = Math.max(out[c], lineWidth(cellMetrics(r, c), rows[r][c], 0))
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
        // ...and when even the widest words don't fit, only the widest
        // ones break: every column keeps its widest word up to a common cap,
        // set so the columns fill the width (a long part number wraps
        // mid-word, "DDR5" and "SIZE" stay whole; Zeb N1).
        const sorted = words.slice().sort((a, b) => a - b)
        let cap = available / n
        let used = 0
        for (let i = 0; i < n; ++i) {
            const c = (available - used) / (n - i)
            if (sorted[i] > c) {
                cap = c
                break
            }
            used += sorted[i]
            cap = sorted[i]
        }
        return words.map(x => Math.min(x, cap))
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
                        // Whole tokens per line; a token wider than the column
                        // is split by characters (root.wrapped).
                        text: root.wrapped(root.headerText(modelData), width, headMetrics, headProbe.font.letterSpacing)
                        kind: "label"
                        font.capitalization: Font.MixedCase
                        wrapMode: Text.NoWrap
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
                            readonly property bool bold: root.boldRows.indexOf(tableRow.index) >= 0
                            // Whole tokens per line; a token wider than the
                            // column is split by characters (root.wrapped).
                            text: root.wrapped(modelData, width, root.cellMetrics(tableRow.index, index), 0)
                            kind: index === 0 ? "body" : "data"
                            font.pixelSize: 13
                            font.weight: bold ? Font.DemiBold : Font.Normal
                            wrapMode: Text.NoWrap
                        }
                    }
                }
            }
        }
    }
}
