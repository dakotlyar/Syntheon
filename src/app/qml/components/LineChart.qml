// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import SynBench

// A hand-built line chart (P14). Data is plain values so it can be tested:
//   points      [{x, y}]
//   logX        x on a log2 axis (latency vs buffer size) or linear (time)
//   xTicks      [{x, label}]
//   yUnit       "ns" or "%"
//   yMax        top of the y axis (0 = pick one)
//   annotations [{x, label}] drawn above the curve
//   logY        y on a log axis (Phase 3 D13: latency from L1 to DRAM),
//               with a grid line per yTicks [{y, label}]; off: the linear
//               axis from 0, five grid lines, as before
//   markers     [{x, y}] open dots on the curve (the RAM test's targets)
Item {
    id: root

    property var points: []
    property bool logX: false
    property var xTicks: []
    property string yUnit
    property real yMax: 0
    property var annotations: []
    property bool logY: false
    property var yTicks: []
    property var markers: []
    property color color: Theme.accent
    property string description

    readonly property real padLeft: 44
    readonly property real padRight: 14
    readonly property real padTop: 26  // room for the unit above the top tick
    readonly property real padBottom: 34
    readonly property real plotW: Math.max(1, width - padLeft - padRight)
    readonly property real plotH: Math.max(1, height - padTop - padBottom)

    function niceMax(v) {
        if (v <= 0) return 1
        var p = Math.pow(10, Math.floor(Math.log(v) / Math.LN10))
        // Ceilings that split into 4 readable ticks (6,000 -> 1,500).
        var steps = [1, 1.2, 1.6, 2, 2.4, 3, 4, 5, 6, 8, 10]
        for (let i = 0; i < steps.length; ++i)
            if (steps[i] * p >= v) return steps[i] * p
        return 10 * p
    }

    readonly property real maxY: {
        if (yMax > 0) return yMax
        var m = 0
        for (let i = 0; i < points.length; ++i) m = Math.max(m, points[i].y)
        return niceMax(m * 1.05)
    }
    // The log axis spans the ticks and the positive points.
    readonly property real logMinY: {
        var m = Infinity
        for (let i = 0; i < points.length; ++i) if (points[i].y > 0) m = Math.min(m, points[i].y)
        for (let j = 0; j < yTicks.length; ++j) m = Math.min(m, yTicks[j].y)
        return isFinite(m) ? m : 1
    }
    readonly property real logMaxY: {
        var m = -Infinity
        for (let i = 0; i < points.length; ++i) m = Math.max(m, points[i].y)
        for (let j = 0; j < yTicks.length; ++j) m = Math.max(m, yTicks[j].y)
        return isFinite(m) && m > logMinY ? m : logMinY * 10
    }
    readonly property real minX: {
        var m = Infinity
        for (let i = 0; i < points.length; ++i) m = Math.min(m, points[i].x)
        for (let j = 0; j < xTicks.length; ++j) m = Math.min(m, xTicks[j].x)
        return isFinite(m) ? m : 0
    }
    readonly property real maxX: {
        var m = -Infinity
        for (let i = 0; i < points.length; ++i) m = Math.max(m, points[i].x)
        for (let j = 0; j < xTicks.length; ++j) m = Math.max(m, xTicks[j].x)
        return isFinite(m) ? m : 1
    }

    function fx(v) {
        var a = logX ? Math.log(Math.max(minX, 1e-9)) / Math.LN2 : minX
        var b = logX ? Math.log(Math.max(maxX, 1e-9)) / Math.LN2 : maxX
        var x = logX ? Math.log(Math.max(v, 1e-9)) / Math.LN2 : v
        return padLeft + (b === a ? 0 : (x - a) / (b - a)) * plotW
    }
    function fy(v) {
        if (!logY)
            return padTop + (1 - Math.max(0, Math.min(maxY, v)) / maxY) * plotH
        // At or below the bottom (0 included): the bottom.
        const lo = Math.log(logMinY), hi = Math.log(logMaxY)
        const y = Math.log(Math.max(logMinY, Math.min(logMaxY, v)))
        return padTop + (1 - (y - lo) / (hi - lo)) * plotH
    }

    readonly property var polyline: {
        var p = []
        for (let i = 0; i < points.length; ++i) p.push(Qt.point(fx(points[i].x), fy(points[i].y)))
        return p
    }
    readonly property var areaPoly: {
        if (polyline.length === 0) return []
        var p = polyline.slice()
        p.push(Qt.point(fx(points[points.length - 1].x), fy(0)))
        p.push(Qt.point(fx(points[0].x), fy(0)))
        return p
    }

    // "1,500", "2.5", "0.25": thousands separators, at most 2 decimals.
    function tickText(v) {
        const r = Math.round(v * 100) / 100
        const decimals = Math.abs(r - Math.round(r)) < 1e-9 ? 0 : Math.abs(r * 10 - Math.round(r * 10)) < 1e-9 ? 1 : 2
        return r.toLocaleString(Qt.locale("en_US"), 'f', decimals)
    }

    SbText { id: noteProbe; visible: false; kind: "data"; font.pixelSize: 11 }
    FontMetrics { id: noteMetrics; font: noteProbe.font }
    // Where each annotation goes: centred on its x (kept inside the plot),
    // just above the highest point of the curve under it (never on the
    // line), and moved up a row while it would overlap a label already
    // placed (a peak mark next to a window label).
    readonly property var notePlacement: {
        const out = []
        const placed = []
        const order = annotations.map((a, i) => i).sort((i, j) => annotations[i].x - annotations[j].x)
        for (const i of order) {
            const a = annotations[i]
            const w = noteMetrics.advanceWidth(a.label) + 2
            const h = noteMetrics.height
            const x = Math.max(padLeft, Math.min(width - padRight - w, fx(a.x) - w / 2))
            let top = fy(0)
            for (const p of points) {
                const px = fx(p.x)
                if (px >= x - 2 && px <= x + w + 2)
                    top = Math.min(top, fy(p.y))
            }
            let y = top - h - 4
            for (let moved = true; moved;) {
                moved = false
                for (const r of placed) {
                    if (x < r.x + r.w + 4 && r.x < x + w + 4 && y < r.y + r.h && r.y < y + h) {
                        y = r.y - h - 1
                        moved = true
                    }
                }
            }
            y = Math.max(0, y)
            placed.push({ x: x, y: y, w: w, h: h })
            out[i] = { x: x, y: y }
        }
        return out
    }

    Accessible.role: Accessible.Graphic
    Accessible.name: description
    implicitHeight: 240

    // Horizontal grid lines and y labels.
    Repeater {
        model: root.logY ? 0 : 5
        delegate: Item {
            id: gridLine
            required property int index
            readonly property real v: root.maxY * index / 4
            Rectangle {
                x: root.padLeft; y: root.fy(gridLine.v); width: root.plotW; height: 1; color: Theme.line
            }
            SbText {
                x: 0; width: root.padLeft - 8; y: root.fy(gridLine.v) - height / 2
                horizontalAlignment: Text.AlignRight
                text: root.tickText(gridLine.v) + (root.yUnit === "%" ? "%" : "")
                kind: "data"; font.pixelSize: 11; color: Theme.muted
            }
        }
    }
    // ...on a log axis, one per tick.
    Repeater {
        model: root.logY ? root.yTicks : []
        delegate: Item {
            id: logLine
            required property var modelData
            Rectangle {
                x: root.padLeft; y: root.fy(logLine.modelData.y); width: root.plotW; height: 1; color: Theme.line
            }
            SbText {
                x: 0; width: root.padLeft - 8; y: root.fy(logLine.modelData.y) - height / 2
                horizontalAlignment: Text.AlignRight
                text: logLine.modelData.label + (root.yUnit === "%" ? "%" : "")
                kind: "data"; font.pixelSize: 11; color: Theme.muted
            }
        }
    }
    SbText {
        x: 2; y: 0
        text: root.yUnit === "%" ? "" : root.yUnit
        kind: "data"; font.pixelSize: 11; color: Theme.muted
    }
    // X ticks.
    Repeater {
        model: root.xTicks
        delegate: SbText {
            required property var modelData
            x: root.fx(modelData.x) - width / 2
            y: root.height - root.padBottom + 14
            text: modelData.label
            kind: "data"; font.pixelSize: 11; color: Theme.muted
        }
    }

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: Qt.rgba(root.color.r, root.color.g, root.color.b, 0.12)
            strokeColor: "transparent"
            PathPolyline { path: root.areaPoly }
        }
        ShapePath {
            fillColor: "transparent"
            strokeColor: root.color
            strokeWidth: 2.2
            joinStyle: ShapePath.RoundJoin
            PathPolyline { path: root.polyline }
        }
    }
    // The last point.
    Rectangle {
        visible: root.points.length > 0
        width: 8; height: 8; radius: 4; color: root.color
        x: (root.points.length ? root.fx(root.points[root.points.length - 1].x) : 0) - 4
        y: (root.points.length ? root.fy(root.points[root.points.length - 1].y) : 0) - 4
    }
    // Open dots on chosen points.
    Repeater {
        model: root.markers
        delegate: Rectangle {
            required property var modelData
            objectName: "marker"
            width: 8; height: 8; radius: 4
            color: Theme.panel
            border.color: root.color; border.width: 2
            x: root.fx(modelData.x) - 4
            y: root.fy(modelData.y) - 4
        }
    }
    Repeater {
        model: root.annotations
        delegate: SbText {
            required property var modelData
            required property int index
            x: root.notePlacement[index] ? root.notePlacement[index].x : 0
            y: root.notePlacement[index] ? root.notePlacement[index].y : 0
            text: modelData.label
            kind: "data"; font.pixelSize: 11
        }
    }
}
