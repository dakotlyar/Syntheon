// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import SynBench

// How the Detail page groups its blocks into rows (Zeb M1): consecutive
// tables pair up unless one is fullWidth. Every CPU table is fullWidth (Dave,
// 2026-10-18), so each is alone on its row, and the all-threads note sits
// right under the Single-thread table.
TestCase {
    id: test
    name: "DetailPage"
    width: 1000
    height: 600
    visible: true
    when: windowShown

    Component { id: pageComponent; DetailPage { width: 900 } }
    Component { id: chartComponent; ChartBlock { width: 600; colorToken: "t-ram" } }

    function table(title, fullWidth) { return { type: "table", title: title, columns: ["A"], rows: [], fullWidth: fullWidth } }
    function note() { return { type: "note", title: "", text: "n", tone: "warn" } }
    function shape(rows) {
        return rows.map(r => r.kind + ":" + r.blocks.map(b => b.title || b.type).join("+"))
    }

    function test_completedUnthrottled() {
        const page = createTemporaryObject(pageComponent, test)
        const blocks = [{ type: "kpis", title: "", items: [] }, table("Sub-tests", true), table("Encryption", true), note(),
                        table("Compression", true), table("Single-thread", true), note()]
        compare(shape(page.group(blocks)), ["single:kpis", "single:Sub-tests", "single:Encryption", "single:note",
                                            "single:Compression", "single:Single-thread", "single:note"])
    }

    function test_stoppedOrWorkerMissing() {
        // No Encryption table and no note: still one table per row.
        const page = createTemporaryObject(pageComponent, test)
        const blocks = [table("Sub-tests", true), table("Compression", true), table("Single-thread", true), note()]
        compare(shape(page.group(blocks)), ["single:Sub-tests", "single:Compression", "single:Single-thread", "single:note"])
    }

    function test_partialWithoutSingleThread() {
        const page = createTemporaryObject(pageComponent, test)
        const blocks = [table("Sub-tests", true), table("Encryption", true), table("Compression", true), note()]
        compare(shape(page.group(blocks)), ["single:Sub-tests", "single:Encryption", "single:Compression", "single:note"])
    }

    function chartBlock(extra) {
        const b = { type: "linechart", title: "Latency", logX: true, yUnit: "ns", annotations: [], colorToken: "t-ram",
                    xTicks: [{ x: 4, label: "4K" }, { x: 1048576, label: "1G" }],
                    points: [{ x: 4, y: 0.8 }, { x: 24, y: 0.9 }, { x: 220, y: 2.6 }, { x: 5836.8, y: 9.8 }, { x: 1048576, y: 92 }] }
        for (const k in extra) b[k] = extra[k]
        return b
    }
    function markers(chart) { return chart.children.filter(c => c.objectName === "marker") }

    // Phase 3 D13: a block without logY / markers (every chart before Phase 3)
    // keeps the linear axis from 0 with its five grid lines and no dots.
    function test_chartDefaultIsLinear() {
        const block = createTemporaryObject(chartComponent, test, { block: chartBlock({}) })
        const chart = findChild(block, "chart")
        verify(!chart.logY)
        compare(markers(chart).length, 0)
        compare(chart.fy(0), chart.padTop + chart.plotH)
        compare(chart.fy(chart.maxY), chart.padTop)
        compare(chart.maxY, 100)  // niceMax(92 x 1.05)
    }

    // The RAM mockup's latency chart: a log y axis from the lowest tick
    // (bottom) to the highest (top), and open dots on the targets.
    function test_chartLogYAndMarkers() {
        const ticks = [0.5, 1, 2, 5, 10, 20, 50, 100, 200].map(v => ({ y: v, label: String(v) }))
        const block = createTemporaryObject(chartComponent, test, {
            block: chartBlock({ logY: true, yTicks: ticks, markers: [{ x: 24, y: 0.9 }, { x: 220, y: 2.6 }, { x: 5836.8, y: 9.8 }] })
        })
        const chart = findChild(block, "chart")
        verify(chart.logY)
        fuzzyCompare(chart.fy(0.5), chart.padTop + chart.plotH, 1e-6)
        fuzzyCompare(chart.fy(200), chart.padTop, 1e-6)
        // Equal ratios are equal distances: 1 -> 10 and 10 -> 100.
        fuzzyCompare(chart.fy(1) - chart.fy(10), chart.fy(10) - chart.fy(100), 1e-6)
        fuzzyCompare(chart.fy(0), chart.fy(0.5), 1e-6)  // zero sits on the floor, not off it
        const dots = markers(chart)
        compare(dots.length, 3)
        for (const d of dots) {
            fuzzyCompare(d.x + 4, chart.fx(d.modelData.x), 1e-6)
            fuzzyCompare(d.y + 4, chart.fy(d.modelData.y), 1e-6)
        }
        verify(dots[0].y > dots[2].y)  // a longer latency sits higher
    }

    // Zeb (checkpoint 1): a latency that wasn't measured (0) would sit on
    // the log floor and look real; on a log axis it isn't drawn at all.
    function test_chartLogYLeavesOutMissingPoints() {
        const ticks = [0.5, 1, 2, 5, 10, 20, 50, 100, 200].map(v => ({ y: v, label: String(v) }))
        const pts = [{ x: 4, y: 0.8 }, { x: 24, y: 0.9 }, { x: 64, y: 0 }, { x: 220, y: 2.6 }, { x: 1048576, y: 92 }]
        const block = createTemporaryObject(chartComponent, test, {
            block: chartBlock({ logY: true, yTicks: ticks, points: pts, markers: [{ x: 24, y: 0.9 }, { x: 64, y: 0 }] })
        })
        const chart = findChild(block, "chart")
        compare(chart.shown.length, 4)
        compare(chart.polyline.length, 4)
        for (const p of chart.polyline)
            verify(p.y < chart.fy(0.5) - 1)  // nothing on the floor
        compare(markers(chart).length, 1)
        // A linear chart keeps every point (a 0 there is a real value).
        const linear = findChild(createTemporaryObject(chartComponent, test, { block: chartBlock({ points: pts }) }), "chart")
        compare(linear.polyline.length, 5)
    }

    // Zeb (checkpoint 1): a cache-size label never touches the curve, also
    // where the line rises steeply across the label between two points.
    function test_annotationsClearTheCurve() {
        const ticks = [0.5, 1, 2, 5, 10, 20, 50, 100, 200].map(v => ({ y: v, label: String(v) }))
        const pts = [{ x: 4, y: 0.7 }, { x: 32, y: 0.71 }, { x: 48, y: 0.75 }, { x: 128, y: 2.4 }, { x: 512, y: 2.5 },
                     { x: 1024, y: 3.5 }, { x: 4096, y: 8.5 }, { x: 32768, y: 9 }, { x: 131072, y: 60 }, { x: 1048576, y: 76 }]
        const notes = [{ x: 48, label: "L1 48 KiB" }, { x: 1024, label: "L2 1 MiB" }, { x: 32768, label: "L3 32 MiB" }]
        const block = createTemporaryObject(chartComponent, test, { block: chartBlock({ logY: true, yTicks: ticks, points: pts, annotations: notes }) })
        const chart = findChild(block, "chart")
        waitForRendering(block)
        const labels = chart.children.filter(c => c.text !== undefined && notes.some(n => n.label === c.text))
        compare(labels.length, 3)
        for (const l of labels) {
            for (let px = l.x; px <= l.x + l.width; px += 1)
                verify(l.y + l.height <= chart.curveYAt(px) + 0.5, l.text + " touches the curve at x " + px)
        }
    }

    // The RAM mockup's rows: each chart beside its table (3:2), a note
    // ending the row; the bandwidth and modules tables full width; the
    // SMBIOS readout beside the derived peak.
    function test_ramMockupRows() {
        const page = createTemporaryObject(pageComponent, test)
        const chart = t => ({ type: "linechart", title: t })
        const blocks = [{ type: "kpis", title: "", items: [] }, table("Sub-tests", true), chart("Latency chart"), table("Latency"), note(),
                        table("Bandwidth", true), note(), chart("Loaded chart"), table("Points"), note(), table("Memory"),
                        table("Peak"), table("Modules", true), note()]
        compare(shape(page.group(blocks)), ["single:kpis", "single:Sub-tests", "split:Latency chart+Latency", "single:note",
                                            "single:Bandwidth", "single:note", "split:Loaded chart+Points", "single:note",
                                            "pair:Memory+Peak", "single:Modules", "single:note"])
    }

    function test_plainTablesStillPair() {
        const page = createTemporaryObject(pageComponent, test)
        compare(shape(page.group([table("A"), table("B")])), ["pair:A+B"])
    }
}
