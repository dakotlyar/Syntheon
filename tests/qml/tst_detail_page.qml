// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import SynBench

// How the Detail page arranges its blocks (Phase 4a D24): the top blocks,
// then the charts, then the tables, collapsed and remembered per test id +
// title for the session (D25); and the chart drawing and hover.
TestCase {
    id: test
    name: "DetailPage"
    width: 1000
    height: 600
    visible: true
    when: windowShown

    Component { id: pageComponent; DetailPage { width: 900 } }
    Component { id: chartComponent; ChartBlock { width: 600; colorToken: "t-ram" } }

    function init() { DetailState.clear() }

    function table(title, fullWidth) { return { type: "table", title: title, columns: ["A"], rows: [], fullWidth: fullWidth } }
    function note(text, tone) { return { type: "note", title: "", text: text, tone: tone } }
    function chart(t) { return { type: "linechart", title: t } }
    function kpis() { return { type: "kpis", title: "", items: [] } }
    function shape(s) {
        const name = b => b.type === "note" ? "note:" + b.text : (b.title || b.type)
        return { top: s.top.map(name),
                 charts: s.charts.map(e => name(e.block) + e.notes.map(n => "+" + n.text).join("")),
                 tables: s.tables.map(e => name(e.block) + e.notes.map(n => "+" + n.text).join("")) }
    }
    function arranged(blocks) {
        return shape(createTemporaryObject(pageComponent, test).arrange(blocks))
    }
    // Every item under `item` with this objectName, in tree order.
    function findAll(item, name) {
        let found = []
        for (const c of item.children) {
            if (c.objectName === name) found.push(c)
            found = found.concat(findAll(c, name))
        }
        return found
    }
    function visibleNotes(page) { return findAll(page, "tableNote").filter(n => n.visible) }

    // Phase 4a D24, §22.3: what each page becomes.
    function test_cpuOrder() {
        const blocks = [kpis(), table("Sub-tests", true), note("throttled", "warn"), table("Encryption", true),
                        note("hw", "neutral"), table("Compression", true), table("Single-thread", true),
                        note("all threads", "neutral"), chart("Clock"), chart("Temp"), note("battery", "warn")]
        compare(arranged(blocks), { top: ["kpis", "note:throttled", "note:battery"], charts: ["Clock", "Temp"],
                                    tables: ["Sub-tests", "Encryption+hw", "Compression", "Single-thread+all threads"] })
    }

    function test_ramOrder() {
        const blocks = [kpis(), table("Sub-tests"), chart("Lat"), table("Latency"), note("caches", "neutral"),
                        table("Bandwidth"), note("stores", "neutral"), note("limited", "warn"), chart("Loaded"),
                        table("Points"), note("chase", "neutral"), table("Memory"), table("Peak"), table("Modules"),
                        note("spd", "neutral"), note("pages", "warn")]
        compare(arranged(blocks), { top: ["kpis", "note:limited", "note:pages"], charts: ["Lat+caches", "Loaded+chase"],
                                    tables: ["Sub-tests", "Latency", "Bandwidth+stores", "Points", "Memory", "Peak",
                                             "Modules+spd"] })
    }

    function test_gpuOrder() {
        const blocks = [kpis(), table("Sub-tests"), table("3D"), note("each", "neutral"), chart("Geo"), table("GeoBy"),
                        chart("DC"), table("DCBy"), note("draws", "neutral"), chart("Sh"), table("ShBy"), chart("Pa"),
                        table("PaBy"), table("Fill"), note("fill", "neutral"), table("Native"), note("native", "neutral"),
                        table("APIs"), table("Render"), note("scaled", "warn")]
        compare(arranged(blocks), { top: ["kpis", "note:scaled"], charts: ["Geo", "DC+draws", "Sh", "Pa"],
                                    tables: ["Sub-tests", "3D+each", "GeoBy", "DCBy", "ShBy", "PaBy", "Fill+fill",
                                             "Native+native", "APIs", "Render"] })
    }

    function test_genericAndNa() {
        const generic = [kpis(), table("All metrics"), chart("s1"), chart("s2"), note("why", "na")]
        compare(arranged(generic), { top: ["kpis", "note:why"], charts: ["s1", "s2"], tables: ["All metrics"] })
        const page = createTemporaryObject(pageComponent, test, { blocks: [{ type: "nastate", title: "N/A", body: "" }] })
        compare(shape(page.sections), { top: ["N/A"], charts: [], tables: [] })
        verify(!findChild(page, "tablesBar").visible)
    }

    function test_noteRules() {
        compare(arranged([note("lead", "neutral"), kpis()]), { top: ["note:lead", "kpis"], charts: [], tables: [] })
        compare(arranged([table("A"), note("1", "neutral"), note("2", "neutral")]), { top: [], charts: [], tables: ["A+1+2"] })
        compare(arranged([table("A"), note("w", "warn"), note("n", "neutral")]), { top: ["note:w"], charts: [], tables: ["A+n"] })
    }

    // Phase 4a D25: collapsed by default, remembered per test id + title.
    function test_tablesStartCollapsedAndRemember() {
        const a = { type: "table", title: "A", columns: ["A"], rows: [["1"]], fullWidth: false }
        const blocks = [a, table("B", false)]
        const page = createTemporaryObject(pageComponent, test, { testId: "gpu", blocks: blocks })
        waitForRendering(page)
        let tables = findAll(page, "detailTable")
        compare(tables.length, 2)
        verify(!tables[0].expanded)
        verify(!tables[1].expanded)
        mouseClick(findChild(tables[0], "tableHeader"))
        verify(tables[0].expanded)
        verify(!tables[1].expanded)
        verify(DetailState.isExpanded("gpu|A|0"))

        const again = createTemporaryObject(pageComponent, test, { testId: "gpu", blocks: blocks })
        tables = findAll(again, "detailTable")
        verify(tables[0].expanded)  // session memory
        verify(!tables[1].expanded)

        const other = createTemporaryObject(pageComponent, test, { testId: "cpu", blocks: blocks })
        verify(!findAll(other, "detailTable")[0].expanded)
    }

    function test_expandAll() {
        const page = createTemporaryObject(pageComponent, test, { testId: "t", blocks: [table("A"), table("B")] })
        waitForRendering(page)
        const button = findChild(page, "expandAll")
        verify(findChild(page, "tablesBar").visible)
        compare(button.text, "Expand all")
        mouseClick(button)
        let tables = findAll(page, "detailTable")
        verify(tables[0].expanded && tables[1].expanded)
        compare(button.text, "Collapse all")
        mouseClick(button)
        verify(!tables[0].expanded && !tables[1].expanded)
        compare(button.text, "Expand all")
        // Opening every table one by one also flips the button.
        tables[0].expandRequested(true)
        compare(button.text, "Expand all")
        tables[1].expandRequested(true)
        compare(button.text, "Collapse all")
    }

    function test_duplicateTitles() {
        const page = createTemporaryObject(pageComponent, test, { testId: "t", blocks: [table("X"), table("X")] })
        compare(page.tableKeys, ["t|X|0", "t|X|1"])
    }

    function test_tableNotesOnlyWhenOpen() {
        const page = createTemporaryObject(pageComponent, test, { testId: "t", blocks: [table("A"), note("n", "neutral")] })
        compare(visibleNotes(page).length, 0)
        findAll(page, "detailTable")[0].expandRequested(true)
        const shown = visibleNotes(page)
        compare(shown.length, 1)
        compare(shown[0].block.text, "n")
    }

    // A chart's notes sit under it, always shown.
    function test_chartNotes() {
        const page = createTemporaryObject(pageComponent, test, { blocks: [chartBlock({}), note("caches", "neutral")] })
        const notes = findAll(page, "chartNote")
        compare(notes.length, 1)
        verify(notes[0].visible)
        compare(notes[0].block.text, "caches")
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

    // AP-5 L1: `filled` moves the filled dot off the last point.
    function test_chartFilledDot() {
        const block = createTemporaryObject(chartComponent, test, { block: chartBlock({ logX: false, points: [{ x: 0, y: 76 }, { x: 64.1, y: 197 }, { x: 66.2, y: 161 }], filled: { x: 64.1, y: 197 } }) })
        const chart = findChild(block, "chart")
        const dot = findChild(chart, "endDot")
        fuzzyCompare(dot.x + 4, chart.fx(64.1), 1e-6)
        fuzzyCompare(dot.y + 4, chart.fy(197), 1e-6)
        chart.filled = null
        fuzzyCompare(dot.x + 4, chart.fx(66.2), 1e-6)
        // On a log axis a filled point at 0 isn't drawn on the floor: the
        // last positive point instead.
        chart.logY = true
        chart.filled = { x: 0, y: 0 }
        fuzzyCompare(dot.x + 4, chart.fx(66.2), 1e-6)
    }

    // Phase 4a D27: the live hover readout.
    function hoverChart(extra) {
        return findChild(createTemporaryObject(chartComponent, test, { block: chartBlock(extra) }), "chart")
    }
    function test_hoverNearestByX() {
        const chart = hoverChart({ logX: false, points: [{ x: 0, y: 1 }, { x: 10, y: 2 }, { x: 20, y: 3 }, { x: 30, y: 4 }], xTicks: [] })
        compare(chart.nearestIndex(chart.fx(12)), 1)
        compare(chart.nearestIndex(chart.fx(16)), 2)
        compare(chart.nearestIndex(chart.fx(15)), 1)  // a tie goes to the earlier point
        compare(chart.nearestIndex(-500), 0)
    }
    function test_hoverLogX() {
        const chart = hoverChart({})
        compare(chart.nearestIndex(chart.fx(5000)), 3)
    }
    function test_hoverSkipsFilteredPoints() {
        const ticks = [{ y: 1, label: "1" }, { y: 10, label: "10" }]
        const pts = [{ x: 4, y: 0.8 }, { x: 24, y: 0.9 }, { x: 64, y: 0 }, { x: 220, y: 2.6 }, { x: 5836.8, y: 9.8 }]
        const chart = hoverChart({ logY: true, yTicks: ticks, points: pts })
        verify(chart.shown[chart.nearestIndex(chart.fx(64))].x !== 64)
    }
    function test_hoverText() {
        const chart = hoverChart({})
        compare(chart.hoverText({ x: 12.35, y: 412, label: "12.35 s · 412 fps" }), "12.35 s · 412 fps")
        compare(chart.hoverText({ x: 2, y: 3.5 }), "2 · 3.5 ns")
        chart.yUnit = "%"
        compare(chart.hoverText({ x: 2, y: 3.5 }), "2 · 3.5%")
    }
    function test_hoverReadoutPos() {
        const chart = hoverChart({})
        let p = chart.readoutPos(100, 100, 120, 20)
        compare(p.x, 110); compare(p.y, 72)
        p = chart.readoutPos(chart.width - 40, 100, 120, 20)
        compare(p.x, chart.width - 40 - 10 - 120); compare(p.y, 72)
        compare(chart.readoutPos(5, 100, 120, 20).x, 15)
        compare(chart.readoutPos(100, 10, 120, 20).y, 20)
    }
    function test_hoverMouse() {
        const chart = hoverChart({})
        waitForRendering(chart)
        mouseMove(chart, chart.fx(24), chart.padTop + 20)
        tryCompare(chart, "hoverIndex", 1)
        verify(findChild(chart, "hoverDot").visible)
        verify(findChild(chart, "hoverGuide").visible)
        verify(findChild(chart, "hoverReadout").visible)
        compare(findChild(chart, "hoverText").text, chart.hoverText(chart.shown[1]))
        mouseMove(chart, 2, chart.padTop + 20)
        tryCompare(chart, "hoverIndex", -1)
        verify(!findChild(chart, "hoverDot").visible)
        verify(!findChild(chart, "hoverGuide").visible)
        verify(!findChild(chart, "hoverReadout").visible)
        mouseMove(chart, chart.fx(24), chart.padTop + 20)
        tryCompare(chart, "hoverIndex", 1)
        mouseMove(test, 1, 1)
        tryCompare(chart, "hoverIndex", -1)
    }
    function test_hoverKeys() {
        const chart = hoverChart({})
        chart.forceActiveFocus()
        compare(chart.shown.length, 5)
        keyClick(Qt.Key_Right); compare(chart.hoverIndex, 0)
        keyClick(Qt.Key_Right); compare(chart.hoverIndex, 1)
        keyClick(Qt.Key_End); compare(chart.hoverIndex, 4)
        keyClick(Qt.Key_Left); compare(chart.hoverIndex, 3)
        compare(chart.Accessible.description, chart.hoverText(chart.shown[3]))
        keyClick(Qt.Key_Home); compare(chart.hoverIndex, 0)
        keyClick(Qt.Key_Escape); compare(chart.hoverIndex, -1)
        keyClick(Qt.Key_Left); compare(chart.hoverIndex, 4)
        test.forceActiveFocus()
        compare(chart.hoverIndex, -1)
    }
    function test_hoverResetsOnNewPoints() {
        const chart = hoverChart({})
        chart.hoverIndex = 2
        chart.points = [{ x: 1, y: 1 }, { x: 2, y: 2 }, { x: 3, y: 3 }]
        compare(chart.hoverIndex, -1)
    }

    // AP-5 L6: the readout never starts left of 0, even when flipping left from near the right edge puts it there.
    function test_hoverReadoutPosClampsAfterFlip() {
        const chart = hoverChart({})
        const p = chart.readoutPos(chart.width - 5, 100, chart.width, 20)  // as wide as the chart: px - 10 - w < 0
        compare(p.x, 0)
        compare(p.y, 72)
    }

    // AP-5 L3: a touch tap uses the mouse's +-8 px zone.
    function test_tapZone() {
        const chart = hoverChart({})
        waitForRendering(chart)
        compare(chart.indexAt(chart.fx(24)), 1)
        compare(chart.indexAt(chart.padLeft - 8), 0)
        compare(chart.indexAt(chart.padLeft - 9), -1)
        compare(chart.indexAt(chart.padLeft + chart.plotW + 8), 4)
        compare(chart.indexAt(chart.padLeft + chart.plotW + 9), -1)
        const tap = (x) => {
            touchEvent(chart).press(0, chart, x, chart.padTop + 20).commit()
            touchEvent(chart).release(0, chart, x, chart.padTop + 20).commit()
        }
        tap(chart.fx(24))
        tryCompare(chart, "hoverIndex", 1)
        tap(chart.fx(24))  // tapping the shown point again hides it
        tryCompare(chart, "hoverIndex", -1)
        tap(chart.padLeft - 20)  // outside the zone: nothing
        wait(50)
        compare(chart.hoverIndex, -1)
    }

    // Zeb D1: the last x tick stays inside the chart box and clear of the focus ring, and so does the unit label.
    function test_lastTickInsideTheChart() {
        const chart = hoverChart({ logX: false, yUnit: "ns", xTicks: [{ x: 0, label: "0 GB/s" }, { x: 150, label: "150 GB/s" }],
                                   points: [{ x: 0, y: 1 }, { x: 75, y: 2 }, { x: 150, y: 3 }] })
        waitForRendering(chart)
        const labels = []
        const scan = (item) => { for (const c of item.children) { if (c.text !== undefined) labels.push(c); scan(c) } }
        scan(chart)
        const tick = labels.filter(l => l.text === "150 GB/s")[0]
        const first = labels.filter(l => l.text === "0 GB/s")[0]
        const unit = labels.filter(l => l.text === "ns")[0]
        verify(tick && first && unit)
        verify(tick.x + tick.width <= chart.width - 4, "last tick right edge " + (tick.x + tick.width) + " of " + chart.width)
        verify(first.x >= 4)
        verify(unit.x >= 4 && unit.y >= 2)
        // a tick that fits is still centred on its position
        const mid = hoverChart({ logX: false, xTicks: [{ x: 0, label: "0" }, { x: 75, label: "75" }, { x: 150, label: "150" }],
                                 points: [{ x: 0, y: 1 }, { x: 150, y: 3 }] })
        waitForRendering(mid)
        const m = []
        const scan2 = (item) => { for (const c of item.children) { if (c.text === "75") m.push(c); scan2(c) } }
        scan2(mid)
        fuzzyCompare(m[0].x + m[0].width / 2, mid.fx(75), 0.5)
    }

    // Zeb D2: a clamped last tick never touches the one before it: on a narrow chart the previous label gives way, on a
    // wide one every label shows. The CPU clock axis (30 s steps, the run's end at 262 s), RAM's and the GPU's.
    function sizedChart(extra, w) {
        const block = createTemporaryObject(chartComponent, test, { block: chartBlock(extra), width: w })
        waitForRendering(block)
        const chart = findChild(block, "chart")
        verify(Math.abs(chart.width - w) < w * 0.2, "chart width " + chart.width + " for a block of " + w)
        return chart
    }
    function test_lastTickNeverTouchesThePreviousOne() {
        const cpu = [0, 30, 60, 90, 120, 150, 180, 210, 240].map(v => ({ x: v, label: v + " s" })).concat([{ x: 262, label: "262 s" }])
        const ram = [0, 25, 50, 75, 100, 125, 150].map(v => ({ x: v, label: v + " GB/s" }))
        const gpu = [0, 5, 10, 15, 20].map(v => ({ x: v, label: v + " s" }))
        for (const ticks of [cpu, ram, gpu]) {
            for (const w of [300, 420, 560, 900]) {
                const chart = sizedChart({ logX: false, xTicks: ticks, points: [{ x: 0, y: 1 }, { x: ticks[ticks.length - 1].x, y: 2 }] }, w)
                const placed = chart.xTickPlaced
                compare(placed.length, ticks.length)
                verify(placed[placed.length - 1].shown, "the last tick shows at " + w)
                let prev = null
                for (const p of placed) {
                    if (!p.shown)
                        continue
                    verify(p.x >= 4 && p.x + p.w <= chart.width - 4, p.label + " inside at " + w)
                    if (prev)
                        verify(prev.x + prev.w + chart.xTickGap <= p.x, prev.label + " touches " + p.label + " at " + w)
                    prev = p
                }
            }
        }
        // Wide enough: nothing is hidden.
        const wide = sizedChart({ logX: false, xTicks: gpu, points: [{ x: 0, y: 1 }, { x: 20, y: 2 }] }, 900)
        verify(wide.xTickPlaced.every(p => p.shown))
        // CPU at a narrow width: "240 s" gives way to "262 s".
        const narrow = sizedChart({ logX: false, xTicks: cpu, points: [{ x: 0, y: 1 }, { x: 262, y: 2 }] }, 420)
        verify(!narrow.xTickPlaced[8].shown && narrow.xTickPlaced[9].shown)
    }

    // A custom block the build can't draw becomes a warning note at the top (AP-5 L6).
    function test_customBlockFallback() {
        const page = createTemporaryObject(pageComponent, test, { blocks: [{ type: "custom", title: "", qmlType: "Nope", data: {} }, kpis()] })
        waitForRendering(page)
        compare(shape(page.sections), { top: ["custom", "kpis"], charts: [], tables: [] })
        const found = []
        const scan = (item) => {
            for (const c of item.children) {
                if (c.block !== undefined && c.block !== null && c.block.text !== undefined) found.push(c)
                scan(c)
            }
        }
        scan(page)
        compare(found.length, 1)
        compare(found[0].block.text, "This view isn't available in this version.")
        compare(found[0].block.tone, "warn")
    }

    // The sections draw top first, then the charts, then the tables bar and the tables (AP-5 L6).
    function test_renderedOrder() {
        const blocks = [table("Late", false), chartBlock({ title: "C1" }), { type: "kpis", title: "", items: [{ label: "L", value: "1", unit: "" }] },
                        chartBlock({ title: "C2" }), note("warned", "warn"), table("Early", false)]
        const page = createTemporaryObject(pageComponent, test, { testId: "order", blocks: blocks })
        waitForRendering(page)
        const yOf = (item) => page.mapFromItem(item, 0, 0).y
        const kpi = findAll(page, "kpiValue")[0]
        const warned = []
        const scan = (item) => { for (const c of item.children) { if (c.block && c.block.text === "warned") warned.push(c); scan(c) } }
        scan(page)
        const charts = findAll(page, "chart")
        const bar = findChild(page, "tablesBar")
        const tables = findAll(page, "detailTable")
        compare(charts.length, 2)
        compare(tables.length, 2)
        compare(warned.length, 1)
        verify(kpi && bar)
        verify(yOf(kpi) < yOf(charts[0]), "KPIs above the charts")
        verify(yOf(warned[0]) < yOf(charts[0]), "the warning above the charts")
        verify(yOf(charts[0]) < yOf(charts[1]), "charts in order")
        verify(yOf(charts[1]) < yOf(bar), "the tables bar below the charts")
        verify(yOf(bar) < yOf(tables[0]), "the tables below the bar")
        verify(yOf(tables[0]) < yOf(tables[1]), "tables in order")
        compare(page.sections.tables[0].block.title, "Late")  // in the order given
    }
}
