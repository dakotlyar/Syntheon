// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import SynBench

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
}
