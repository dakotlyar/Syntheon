// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import Syntheon
import SyntheonTest

// The Detail page's table: columns sized to their content (Dave, 2026-10-18).
// Wide: nothing wraps and the first column takes the spare room. Narrow (the
// tabs layout on a small window): the columns still fit the table and the
// cells wrap instead of overlapping.
TestCase {
    id: test
    name: "TableBlock"
    width: 1000
    height: 600
    visible: true
    when: windowShown

    Component { id: tableComponent; TableBlock { } }
    Component { id: spyComponent; SignalSpy { } }

    readonly property var cpuBlock: ({
        type: "table", title: "Sub-tests (all threads)",
        columns: ["Group", "Weight", "Result", "Sub-score"],
        rows: [["Integer", "25%", "155,708 MOps/s", "973"],
               ["Encryption (hardware path)", "10%", "AES-256-GCM 254,639 MB/s\nSHA-256 40,821 MB/s", "1,058"],
               ["CPU score", "100%", "weighted geometric mean", "1,026"]]
    })

    function sum(a) { return a.reduce((x, y) => x + y, 0) }

    function test_wideFitsWithoutWrapping() {
        const t = createTemporaryObject(tableComponent, test, { block: cpuBlock, width: 880 })
        waitForRendering(t)
        compare(t.widths.length, 4)
        for (let c = 1; c < 4; ++c)
            verify(t.widths[c] >= t.needs[c], "column " + c + " is narrower than its widest line")
        verify(t.widths[0] >= t.needs[0])
        verify(Math.abs(sum(t.widths) - t.available) < 1)
    }

    function test_narrowStillFits() {
        for (const w of [520, 360, 240]) {
            const t = createTemporaryObject(tableComponent, test, { block: cpuBlock, width: w })
            waitForRendering(t)
            verify(sum(t.widths) <= t.available + 1, "too wide at " + w)
            for (const x of t.widths)
                verify(x > 0)
        }
    }

    function test_twoColumns() {
        const t = createTemporaryObject(tableComponent, test,
                                        { block: { type: "table", title: "", columns: ["Metric", "Value"],
                                                   rows: [["Read bandwidth", "78.4 GB/s"]] }, width: 400 })
        waitForRendering(t)
        verify(t.widths[1] >= t.needs[1])
        verify(t.widths[0] > t.widths[1])
    }

    // Zeb's Defect B, without an extra glyph (round 4, C1): a cell breaks
    // only at its spaces and line breaks, and a long identifier also after a
    // hyphen before a letter; never inside "GB/s", "SUB-SCORE", "128-bit",
    // "F5-6000J3038F16G" or an underscore name.
    function test_wrapKeepsUnitsWhole() {
        const t = createTemporaryObject(tableComponent, test, { block: cpuBlock, width: 600 })
        waitForRendering(t)
        const words = s => t.tokens(s).map(x => x.text)
        compare(words("67.1 GB/s"), ["67.1", "GB/s"])
        compare(words("SUB-SCORE"), ["SUB-SCORE"])
        compare(words("6,000 MT/s × 128-bit ÷ 8"), ["6,000", "MT/s", "×", "128-bit", "÷", "8"])
        compare(words("F5-6000J3038F16G"), ["F5-6000J3038F16G"])
        compare(words("Socket0_Controller0_ChannelA_DIMM1"), ["Socket0_Controller0_ChannelA_DIMM1"])
        compare(words("Controller0-ChannelA-DIMM0"), ["Controller0-", "ChannelA-", "DIMM0"])
        // Wrapping: whole tokens per line, hyphen pieces joined without a space.
        const m = t.tokenMetrics
        const narrow = t.wordWidth(m, "Controller0-", 0)
        compare(t.wrapped("67.1 GB/s", t.wordWidth(m, "GB/s", 0), m, 0), "67.1\nGB/s")
        compare(t.wrapped("Controller0-ChannelA-DIMM0", narrow, m, 0), "Controller0-\nChannelA-\nDIMM0")
        compare(t.wrapped("Controller0-ChannelA-DIMM0 (BANK 0)", 10000, m, 0), "Controller0-ChannelA-DIMM0 (BANK 0)")
        compare(t.wrapped("a\nb c", 10000, m, 0), "a\nb c")
        // The sizing uses the same tokens (AP-5 L6): the widest is "GB/s".
        compare(t.wordWidth(m, "1 GB/s", 0), Math.ceil(m.advanceWidth("GB/s")) + 2)
    }

    // AP-5 round 4 M1: a token wider than the column on its own is split by
    // characters, and only that token: the short words around it stay whole.
    function test_overlongTokenSplitsAlone() {
        const t = createTemporaryObject(tableComponent, test, { block: cpuBlock, width: 600 })
        waitForRendering(t)
        const m = t.tokenMetrics
        const part = "MTC10F1084S1RC48BA1"
        const w = t.wordWidth(m, "Technology", 0)  // narrower than the part number
        verify(t.measure(m, part, 0) > w)
        const out = t.wrapped("Micron Technology " + part + " (16 GB)", w, m, 0)
        const lines = out.split("\n")
        verify(lines.indexOf("Micron") >= 0 || lines.indexOf("Micron Technology") >= 0, out)
        verify(lines.indexOf("Technology") >= 0 || lines.indexOf("Micron Technology") >= 0, out)
        verify(out.indexOf("(16 GB)") >= 0 || (lines.indexOf("(16") >= 0 && lines.indexOf("GB)") >= 0), out)
        // The part number's pieces, in order, each within the column.
        compare(out.replace(/\n/g, "").replace(/ /g, ""), ("MicronTechnology" + part + "(16GB)"))
        for (const line of lines)
            verify(t.measure(m, line, 0) <= w + 1, "'" + line + "' is wider than " + w)
        // Headers take the same path (no null fallback any more).
        compare(t.wrapped("", w, m, 0), "")
    }

    // AP-5 round 4 M2: a bold row is sized with the bold metrics it is drawn
    // with, so a bold cell that fits doesn't wrap. (The data font is
    // monospaced, the same width bold; the first column's body font isn't.)
    function test_boldRowsSizedBold() {
        const label = "Weighted geometric mean"
        const block = { type: "table", title: "", columns: ["Group", "Result"],
                        rows: [["Read", "6.1 GB/s"], [label, "67.1 GB/s"]], boldRows: [1] }
        const t = createTemporaryObject(tableComponent, test, { block: block, width: 600 })
        waitForRendering(t)
        compare(t.cellMetrics(1, 0).font.weight, Font.DemiBold)  // the bold row, as drawn
        compare(t.cellMetrics(0, 0).font.weight, Font.Normal)
        const bold = t.lineWidth(t.cellMetrics(1, 0), label, 0)
        verify(bold > t.lineWidth(t.cellMetrics(0, 0), label, 0), "DemiBold body should be wider")
        verify(t.needs[0] >= bold)
        verify(t.compactNeeds[0] >= bold)
        verify(t.wordNeeds[0] >= t.wordWidth(t.cellMetrics(1, 0), "geometric", 0))
        // Exactly as wide as the table needs: the bold label stays on one line.
        const exact = createTemporaryObject(tableComponent, test, { block: block, width: 24 + t.gap + t.needs[0] + t.needs[1] })
        waitForRendering(exact)
        compare(exact.wrapped(label, exact.widths[0], exact.cellMetrics(1, 0), 0), label)
    }

    // Phase 4a M1: one cell in bold, not its row; sized with the bold metrics.
    function boldCellItems(t) {
        const out = []
        const walk = item => {
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.bold !== undefined && c.text !== undefined)
                    out.push(c)
                walk(c)
            }
        }
        walk(t)
        return out
    }

    function test_boldCellsOnlyThatCell() {
        const block = { type: "table", title: "", columns: ["Scene", "Vulkan", "DX12"],
                        rows: [["Geometry", "410 fps", "412 fps"], ["Shading", "205 fps", "199 fps"], ["Particles", "88 fps", "90 fps"]],
                        boldCells: [[2, 1]] }
        const t = createTemporaryObject(tableComponent, test, { block: block, width: 600 })
        waitForRendering(t)
        const cells = boldCellItems(t)
        compare(cells.length, 9)
        let bold = 0
        for (const c of cells) {
            if (c.font.weight === Font.DemiBold) {
                ++bold
                compare(c.text, "199 fps")
            }
        }
        compare(bold, 1)
        compare(t.cellMetrics(1, 2).font.weight, Font.DemiBold)
        compare(t.cellMetrics(1, 1).font.weight, Font.Normal)
        verify(t.needs[2] >= t.lineWidth(t.cellMetrics(1, 2), "199 fps", 0))
        // A bold cell in the proportional first column is sized with the bold metrics.
        const first = { type: "table", title: "", columns: ["Group", "Result"],
                        rows: [["Read", "6.1 GB/s"], ["Weighted geometric mean", "67.1 GB/s"]], boldCells: [[0, 1]] }
        const f = createTemporaryObject(tableComponent, test, { block: first, width: 600 })
        waitForRendering(f)
        compare(f.cellMetrics(1, 0).font.weight, Font.DemiBold)
        compare(f.cellMetrics(0, 0).font.weight, Font.Normal)
        verify(f.needs[0] >= f.lineWidth(f.cellMetrics(1, 0), "Weighted geometric mean", 0))
    }

    // Zeb D1: the same through the C++ export (buildDetailBlocks), whose boldCells once arrived flattened.
    function test_boldCellsFromTheCppExport() {
        const block = Fixtures.exportedBoldTable()
        compare(block.boldCells.length, 1)
        compare(block.boldCells[0].length, 2)
        const t = createTemporaryObject(tableComponent, test, { block: block, width: 600 })
        waitForRendering(t)
        verify(t.isBold(1, 2))
        verify(!t.isBold(2, 1))
        let bold = 0
        for (const c of boldCellItems(t)) {
            if (c.font.weight === Font.DemiBold) {
                ++bold
                compare(c.text, "199 fps")
            }
        }
        compare(bold, 1)
    }

    // Zeb D4: the GPU page's APIs table. Its text columns are left-aligned, and on a narrow window Driver and Note wrap
    // while API, Version and Present keep one line; on a wide one the last wrap column (Note) takes the spare room.
    readonly property var apisBlock: ({
        type: "table", title: "APIs", columns: ["API", "Status", "Version", "Driver", "Present", "Note"],
        rows: [["DirectX 12", "OK", "FL 12_1 · SM 6.8", "32.0.21025.10", "flip, tearing", "—"],
               ["Vulkan", "OK", "Vulkan 1.4.354", "amdgpu · Mesa 26.2.4-arch3.1 (LLVM 21.1.0, 256 bits)", "IMMEDIATE", "—"],
               ["Qt raster", "OK", "Qt 6.12.0", "—", "backing store", "CPU path, scored · the bundled font isn't in the worker; Segoe UI instead"]],
        leftColumns: [1, 2, 3, 4, 5], wrapColumns: [3, 5] })
    function test_textAndWrapColumns() {
        for (const w of [640, 760, 1100]) {
            const t = createTemporaryObject(tableComponent, test, { block: apisBlock, width: w })
            waitForRendering(t)
            verify(Math.abs(sum(t.widths) - t.available) < 1, "the columns fill the table at " + w)
            for (const c of [0, 2, 4])
                verify(t.widths[c] >= t.needs[c] - 0.5, "column " + c + " keeps one line at " + w + " (" + t.widths[c] + " < " + t.needs[c] + ")")
            if (w >= 1000) {
                for (const c of [0, 1, 2, 4])
                    compare(t.widths[c], t.needs[c])  // exactly one line each; Driver and Note share the rest
            }
            const cells = boldCellItems(t)
            for (const c of cells) {
                if (c.text === "Vulkan 1.4.354" || c.text === "IMMEDIATE")
                    compare(c.horizontalAlignment, Text.AlignLeft)
            }
        }
        // Wide enough for every line: the spare room goes to the last wrap column (Note), not to API.
        const roomy = createTemporaryObject(tableComponent, test, {
            block: Object.assign({}, apisBlock, { rows: apisBlock.rows.map(r => r.slice(0, 5).concat(["—"])) }), width: 1100 })
        waitForRendering(roomy)
        for (const c of [0, 1, 2, 3, 4])
            compare(roomy.widths[c], roomy.needs[c])
        verify(roomy.widths[5] > roomy.needs[5])
        // Without the new fields the table is sized and aligned as before.
        const plain = createTemporaryObject(tableComponent, test, { block: Object.assign({}, apisBlock, { leftColumns: [], wrapColumns: [] }), width: 760 })
        waitForRendering(plain)
        verify(!plain.isText(2))
    }

    // A table without boldCells (or with an empty list) is sized exactly as before.
    function test_noBoldCellsSameWidths() {
        const rows = [["Geometry", "410 fps", "412 fps"], ["Shading", "205 fps", "199 fps"]]
        const plain = createTemporaryObject(tableComponent, test, { block: { type: "table", title: "", columns: ["Scene", "Vulkan", "DX12"], rows: rows }, width: 600 })
        const empty = createTemporaryObject(tableComponent, test, { block: { type: "table", title: "", columns: ["Scene", "Vulkan", "DX12"], rows: rows, boldCells: [] }, width: 600 })
        waitForRendering(plain)
        waitForRendering(empty)
        compare(empty.widths, plain.widths)
        for (const c of boldCellItems(plain))
            compare(c.font.weight, Font.Normal)
    }

    // AP-5 round 4 L7: screen readers get a cell's text unwrapped, not the
    // lines the table broke it into. L8: the memos give the same results.
    function test_accessibleNamesUnwrapped() {
        const label = "Controller0-ChannelA-DIMM0 (BANK 0)"
        const block = { type: "table", title: "", columns: ["Slot", "Part number"],
                        rows: [[label, "MTC10F1084S1RC48BA1"]] }
        const t = createTemporaryObject(tableComponent, test, { block: block, width: 260 })
        waitForRendering(t)
        const cells = []
        const walk = item => {
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.text !== undefined && c.Accessible && (c.text === label || c.Accessible.name === label))
                    cells.push(c)
                walk(c)
            }
        }
        walk(t)
        verify(cells.length > 0, "the slot cell")
        const cell = cells[0]
        verify(cell.text.indexOf("\n") > 0, "the cell wraps at 260 px: " + JSON.stringify(cell.text))
        compare(cell.Accessible.name, label)
        const m = t.tokenMetrics
        compare(t.measure(m, "GB/s", 0), m.advanceWidth("GB/s"))
        compare(t.measure(m, "GB/s", 0), m.advanceWidth("GB/s"))  // from the memo
        compare(t.tokens("a b"), t.tokens("a b"))
    }
    // Phase 4a D25: the collapsible header.
    function collapsibleTable(extra) {
        const props = { block: cpuBlock, width: 880 }
        for (const k in extra) props[k] = extra[k]
        return createTemporaryObject(tableComponent, test, props)
    }
    // A table that isn't collapsible is the table of Phase 3: its title, column header and every row, whatever `expanded` says
    // (the property belongs to the collapsible case), and none of the header button.
    function test_notCollapsibleIsUnchanged() {
        for (const extra of [{}, { collapsible: false, expanded: false }]) {
            const t = collapsibleTable(extra)
            waitForRendering(t)
            verify(!findChild(t, "tableHeader").visible)
            compare(t.open, true)
            compare(t.widths.length, 4)
            let titleSeen = false
            for (const c of t.children[t.children.length - 1].children)
                if (c.text === "Sub-tests (all threads)" && c.visible) titleSeen = true
            verify(titleSeen)
            const rowText = findAllText(t).filter(c => c.text === "973")
            compare(rowText.length, 1)
            verify(rowText[0].visible)
            // Its height is the title, the column header and the rows: more than the collapsed 42 px by far.
            verify(t.implicitHeight > 42 + 32)
        }
        compare(collapsibleTable({}).implicitHeight, collapsibleTable({ expanded: false }).implicitHeight)
    }
    function test_collapsedIsJustTheHeader() {
        const t = collapsibleTable({ collapsible: true, expanded: false })
        waitForRendering(t)
        compare(t.implicitHeight, 42)
        verify(findChild(t, "tableHeader").visible)
        compare(findChild(t, "chevron").path, "M6 4l4 4-4 4")
        // AP-5 L2: nothing of the table is built while it is collapsed (no rows, no column heads, nothing measured).
        compare(findAllText(t).filter(c => c.text === "973").length, 0)
        compare(findAllText(t).filter(c => c.text === "GROUP").length, 0)
        compare(t.widths.length, 0)
        compare(t.open, false)
        t.expanded = true
        waitForRendering(t)
        compare(findAllText(t).filter(c => c.text === "973").length, 1)
        compare(t.widths.length, 4)
        t.expanded = false
        compare(findAllText(t).filter(c => c.text === "973").length, 0)
        tryCompare(t, "implicitHeight", 42)
    }
    function findAllText(item) {
        let out = []
        for (const c of item.children) {
            if (c.text !== undefined) out.push(c)
            out = out.concat(findAllText(c))
        }
        return out
    }
    function test_expandedShowsTheTable() {
        const t = collapsibleTable({ collapsible: true, expanded: true })
        waitForRendering(t)
        verify(t.implicitHeight > 42 + 32)
        const rowText = findAllText(t).filter(c => c.text === "973")
        compare(rowText.length, 1)
        verify(rowText[0].visible)
        compare(findChild(t, "chevron").path, "M4 6l4 4 4-4")
    }
    function test_clickRequests() {
        const t = collapsibleTable({ collapsible: true, expanded: false })
        waitForRendering(t)
        const spy = createTemporaryObject(spyComponent, test, { target: t, signalName: "expandRequested" })
        mouseClick(findChild(t, "tableHeader"))
        compare(spy.count, 1)
        compare(spy.signalArguments[0][0], true)
        verify(!t.expanded)  // the block didn't flip itself
        t.expanded = true
        mouseClick(findChild(t, "tableHeader"))
        compare(spy.count, 2)
        compare(spy.signalArguments[1][0], false)
    }
    function test_keysRequest() {
        const t = collapsibleTable({ collapsible: true, expanded: false })
        waitForRendering(t)
        const spy = createTemporaryObject(spyComponent, test, { target: t, signalName: "expandRequested" })
        findChild(t, "tableHeader").forceActiveFocus()
        keyClick(Qt.Key_Space)
        compare(spy.count, 1)
        keyClick(Qt.Key_Return)
        compare(spy.count, 2)
    }
    function test_summaryAndTitle() {
        const withSummary = { type: "table", title: "Sub-tests (all threads)", summary: "Vulkan 412 fps · 1% low 354",
                              columns: ["A"], rows: [["x"]] }
        const t = collapsibleTable({ block: withSummary, collapsible: true, expanded: false })
        compare(findChild(t, "headerSummary").text, "Vulkan 412 fps · 1% low 354")
        compare(findChild(t, "tableHeader").Accessible.name, "Sub-tests (all threads), Vulkan 412 fps · 1% low 354")
        const plain = collapsibleTable({ collapsible: true, expanded: false })
        compare(findChild(plain, "headerSummary").text, "")
        const untitled = collapsibleTable({ block: { type: "table", title: "", columns: ["A"], rows: [["x"]] }, collapsible: true })
        compare(findChild(untitled, "headerTitle").text, "Details")
    }
    // AP-5 L4 / nit: the summary elides first, the chevron keeps its 16 px inside the header, the title is capped at what is left after
    // the chevron and the two gaps, and the hover fill / focus ring stay inside the Box's border.
    readonly property var longSummary: ({ type: "table", title: "Sub-tests (all threads)",
                                          summary: "Vulkan 412 fps · 1% low 354 · plus a very long tail that cannot fit in a narrow window",
                                          columns: ["A"], rows: [["x"]] })
    function headerParts(width) {
        const t = collapsibleTable({ block: longSummary, collapsible: true, expanded: false, width: width })
        waitForRendering(t)
        const header = findChild(t, "tableHeader")
        return { t: t, header: header, title: findChild(t, "headerTitle"), summary: findChild(t, "headerSummary"),
                 chevron: findChild(t, "chevron") }
    }
    function test_headerSummaryElidesFirst() {
        const p = headerParts(420)
        fuzzyCompare(p.title.width, p.title.implicitWidth, 1)  // the title is whole
        verify(p.summary.width < p.summary.implicitWidth)  // the summary gave way
        verify(p.summary.truncated)
        compare(p.chevron.width, 16)
        const right = p.chevron.mapToItem(p.header, p.chevron.width, 0).x
        verify(right <= p.header.width - p.header.rightPadding + 0.5, "chevron right edge " + right)
    }
    function test_headerNarrowKeepsChevronAndCapsTitle() {
        for (const w of [200, 120, 80]) {
            const p = headerParts(w)
            compare(p.chevron.width, 16)
            const right = p.chevron.mapToItem(p.header, p.chevron.width, 0).x
            verify(right <= p.header.width - p.header.rightPadding + 0.5, "chevron right edge " + right + " at " + w)
            verify(p.title.width <= Math.max(0, p.header.availableWidth - 16 - 20) + 0.5, "title " + p.title.width + " at " + w)
            verify(p.summary.width >= 0)
            // the title, two gaps and the chevron never exceed the header's content width
            verify(p.title.width + 20 + p.chevron.width <= p.header.availableWidth + 0.5)
        }
    }
    function test_headerHoverStaysInsideTheBorder() {
        const p = headerParts(420)
        compare(p.header.background.x, 1)
        compare(p.header.background.y, 1)
        compare(p.header.background.width, p.header.width - 2)
        compare(p.header.background.height, p.header.height - 2)
    }
}
