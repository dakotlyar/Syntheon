// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import Syntheon

// A table of already-formatted cells; the first column (and any leftColumns)
// left in the body font, the rest right in the data font. Columns are sized to
// their content (the widest header or cell line, a cell may hold several
// lines): the value columns get what they need, the first column the rest.
// When even that doesn't fit (a narrow window), the headers wrap at their
// spaces first, then the first column gives way (down to 30%), then every
// column shrinks in proportion and the cells wrap. A table with wrapColumns
// gives them the spare room instead, and on a narrow window only they wrap
// while every other column keeps one line, as long as that fits.
Box {
    id: root

    property var block
    // Phase 4a D25: a collapsible table shows a header button; the page owns
    // `expanded` (the block never sets it) and listens to expandRequested.
    property bool collapsible: false
    property bool expanded: true
    signal expandRequested(bool on)
    readonly property string summary: block && block.summary ? block.summary : ""
    readonly property string headerTitle: block && block.title.length > 0 ? block.title : qsTr("Details")
    readonly property var columns: block ? block.columns : []
    readonly property var rows: block ? block.rows : []
    readonly property var boldRows: block && block.boldRows ? block.boldRows : []
    // Single cells in bold, as [column, row] pairs (Phase 4a M1).
    readonly property var boldCells: block && block.boldCells ? block.boldCells : []
    function isBold(row, c) {
        return boldRows.indexOf(row) >= 0 || boldCells.some(p => p[0] === c && p[1] === row)
    }
    // Text columns: left-aligned in the body font, as the first one (Zeb D4: the GPU page's APIs table).
    readonly property var leftColumns: block && block.leftColumns ? block.leftColumns : []
    function isText(c) { return c === 0 || leftColumns.indexOf(c) >= 0 }
    // The columns that wrap first on a narrow table and take the spare room on a wide one (empty: the first column).
    readonly property var wrapColumns: block && block.wrapColumns ? block.wrapColumns : []

    // Whether the table's rows exist: a collapsed table builds, measures and wraps nothing (AP-5 L2).
    readonly property bool open: !collapsible || expanded
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
    // Tokens per line and widths per text and font, kept across the many
    // re-wraps of a resize (AP-5 round 4 L8; plain memos, the same results).
    readonly property var memo: ({ tokens: new Map(), widths: new Map() })
    function tokens(line) {
        const key = String(line)
        let out = memo.tokens.get(key)
        if (out === undefined) {
            out = splitTokens(key)
            memo.tokens.set(key, out)
        }
        return out
    }
    function splitTokens(line) {
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
        const f = metrics.font
        const key = f.family + "|" + f.weight + "|" + f.pixelSize + "|" + f.pointSize + "|" + f.capitalization + "|" + spacing + "|" + text
        let w = memo.widths.get(key)
        if (w === undefined) {
            w = metrics.advanceWidth(text) + spacing * text.length
            memo.widths.set(key, w)
        }
        return w
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
        const bold = isBold(row, c)
        return isText(c) ? (bold ? bodyBoldMetrics : bodyMetrics) : (bold ? dataBoldMetrics : dataMetrics)
    }
    // What each column needs when its headers wrap: their widest word, and
    // the widest cell line.
    readonly property var compactNeeds: {
        if (!open) return []
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
        if (!open) return []
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
        if (!open) return []
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
        const wrap = wrapColumns.filter(c => c >= 0 && c < n)
        if (total <= available) {
            const w = need.slice()
            w[wrap.length > 0 ? wrap[wrap.length - 1] : 0] += available - total  // the first (or last wrap) column takes the spare room
            return w
        }
        // Wrap columns (Zeb D4): the other columns keep one line for every cell (their headers may wrap at a space), and
        // the wrap columns share what is left, each at least its widest word, in proportion to what each still wants.
        if (wrap.length > 0) {
            for (const fixedNeeds of [need, compactNeeds]) {
                let fixed = 0, words = 0, want = 0
                for (let c = 0; c < n; ++c) {
                    if (wrap.indexOf(c) >= 0) {
                        words += wordNeeds[c]
                        want += Math.max(0, need[c] - wordNeeds[c])
                    } else {
                        fixed += fixedNeeds[c]
                    }
                }
                if (fixed + words <= available) {
                    const spare = available - fixed - words
                    return fixedNeeds.map((x, c) => wrap.indexOf(c) < 0 ? x
                        : wordNeeds[c] + (want > 0 ? Math.max(0, need[c] - wordNeeds[c]) * spare / want : spare / wrap.length))
                }
            }
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

        AbstractButton {
            id: header
            objectName: "tableHeader"
            visible: root.collapsible
            Layout.fillWidth: true
            implicitHeight: 42
            leftPadding: 12; rightPadding: 12
            hoverEnabled: true
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: root.headerTitle + (root.summary.length > 0 ? ", " + root.summary : "")
            Accessible.description: root.expanded ? qsTr("expanded") : qsTr("collapsed")
            onClicked: root.expandRequested(!root.expanded)
            Keys.onReturnPressed: root.expandRequested(!root.expanded)
            Keys.onEnterPressed: root.expandRequested(!root.expanded)
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            // Inside the Box's 1 px border, which the hover fill and the focus ring must not cover.
            topInset: 1; bottomInset: 1; leftInset: 1; rightInset: 1
            background: Rectangle {
                radius: 6
                color: header.hovered ? Theme.sunk : "transparent"
                border.color: header.visualFocus ? Theme.accent : "transparent"
                border.width: 1.5
            }
            // The summary elides first: the title keeps its width up to what is left after the chevron and the two gaps, the
            // summary takes the rest (down to nothing), and the chevron never shrinks (AP-5 L4).
            contentItem: RowLayout {
                spacing: 10
                SbText {
                    objectName: "headerTitle"
                    text: root.headerTitle
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    Layout.maximumWidth: Math.max(0, header.availableWidth - 16 - 2 * 10)
                }
                SbText {
                    objectName: "headerSummary"
                    text: root.summary
                    kind: "small"
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
                Icon {
                    objectName: "chevron"
                    path: root.expanded ? "M4 6l4 4 4-4" : "M6 4l4 4-4 4"
                    color: Theme.muted
                    Layout.minimumWidth: 16; Layout.preferredWidth: 16; Layout.maximumWidth: 16
                    Layout.preferredHeight: 16
                }
            }
        }

        SbText {
            visible: !root.collapsible && root.block && root.block.title.length > 0
            Layout.margins: 12
            text: root.block ? root.block.title : ""
            font.weight: Font.DemiBold
        }

        // header
        Rectangle {
            visible: root.open
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
                    model: root.open ? root.columns : []
                    delegate: SbText {
                        required property string modelData
                        required property int index
                        width: root.columnWidth(index)
                        horizontalAlignment: root.isText(index) ? Text.AlignLeft : Text.AlignRight
                        // Capitals by hand: automatic capitalisation would turn
                        // "µs" into a Greek capital Mu and "GB/s" into "GB/S".
                        // Whole tokens per line; a token wider than the column
                        // is split by characters (root.wrapped).
                        text: root.wrapped(root.headerText(modelData), width, headMetrics, headProbe.font.letterSpacing)
                        Accessible.name: root.headerText(modelData)  // unwrapped (AP-5 round 4 L7)
                        kind: "label"
                        font.capitalization: Font.MixedCase
                        wrapMode: Text.NoWrap
                    }
                }
            }
        }
        ColumnLayout {
            spacing: 0
            Layout.fillWidth: true
            visible: root.open
            Repeater {
                model: root.open ? root.rows : []
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
                                horizontalAlignment: root.isText(index) ? Text.AlignLeft : Text.AlignRight
                                readonly property bool bold: root.isBold(tableRow.index, index)
                                // Whole tokens per line; a token wider than the
                                // column is split by characters (root.wrapped).
                                text: root.wrapped(modelData, width, root.cellMetrics(tableRow.index, index), 0)
                                Accessible.name: modelData  // unwrapped (AP-5 round 4 L7)
                                kind: root.isText(index) ? "body" : "data"
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
}
