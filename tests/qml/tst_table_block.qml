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
}
