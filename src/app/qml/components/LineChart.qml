// SPDX-License-Identifier: GPL-3.0-or-later
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
Item {
    id: root

    property var points: []
    property bool logX: false
    property var xTicks: []
    property string yUnit
    property real yMax: 0
    property var annotations: []
    property color color: Theme.accent
    property string description

    readonly property real left: 44
    readonly property real right: 14
    readonly property real top: 14
    readonly property real bottom: 34
    readonly property real plotW: Math.max(1, width - left - right)
    readonly property real plotH: Math.max(1, height - top - bottom)

    function niceMax(v) {
        if (v <= 0) return 1
        var p = Math.pow(10, Math.floor(Math.log(v) / Math.LN10))
        var steps = [1, 2, 2.5, 5, 10]
        for (var i = 0; i < steps.length; ++i)
            if (steps[i] * p >= v) return steps[i] * p
        return 10 * p
    }

    readonly property real maxY: {
        if (yMax > 0) return yMax
        var m = 0
        for (var i = 0; i < points.length; ++i) m = Math.max(m, points[i].y)
        return niceMax(m * 1.05)
    }
    readonly property real minX: {
        var m = Infinity
        for (var i = 0; i < points.length; ++i) m = Math.min(m, points[i].x)
        for (var j = 0; j < xTicks.length; ++j) m = Math.min(m, xTicks[j].x)
        return isFinite(m) ? m : 0
    }
    readonly property real maxX: {
        var m = -Infinity
        for (var i = 0; i < points.length; ++i) m = Math.max(m, points[i].x)
        for (var j = 0; j < xTicks.length; ++j) m = Math.max(m, xTicks[j].x)
        return isFinite(m) ? m : 1
    }

    function fx(v) {
        var a = logX ? Math.log(Math.max(minX, 1e-9)) / Math.LN2 : minX
        var b = logX ? Math.log(Math.max(maxX, 1e-9)) / Math.LN2 : maxX
        var x = logX ? Math.log(Math.max(v, 1e-9)) / Math.LN2 : v
        return left + (b === a ? 0 : (x - a) / (b - a)) * plotW
    }
    function fy(v) { return top + (1 - Math.max(0, Math.min(maxY, v)) / maxY) * plotH }

    readonly property var polyline: {
        var p = []
        for (var i = 0; i < points.length; ++i) p.push(Qt.point(fx(points[i].x), fy(points[i].y)))
        return p
    }
    readonly property var areaPoly: {
        if (polyline.length === 0) return []
        var p = polyline.slice()
        p.push(Qt.point(fx(points[points.length - 1].x), fy(0)))
        p.push(Qt.point(fx(points[0].x), fy(0)))
        return p
    }

    Accessible.role: Accessible.Graphic
    Accessible.name: description
    implicitHeight: 240

    // Horizontal grid lines and y labels.
    Repeater {
        model: 5
        delegate: Item {
            required property int index
            readonly property real v: root.maxY * index / 4
            Rectangle {
                x: root.left; y: root.fy(v); width: root.plotW; height: 1; color: Theme.line
            }
            SbText {
                x: 0; width: root.left - 8; y: root.fy(v) - height / 2
                horizontalAlignment: Text.AlignRight
                text: Math.round(v * 10) / 10 + (root.yUnit === "%" ? "%" : "")
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
            y: root.height - root.bottom + 14
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
    Repeater {
        model: root.annotations
        delegate: SbText {
            required property var modelData
            readonly property real nearestY: {
                var best = 0, d = Infinity
                for (var i = 0; i < root.points.length; ++i) {
                    var dx = Math.abs(root.points[i].x - modelData.x)
                    if (dx < d) { d = dx; best = root.points[i].y }
                }
                return best
            }
            x: root.fx(modelData.x) - width / 2
            y: Math.max(0, root.fy(nearestY) - 20)
            text: modelData.label
            kind: "data"; font.pixelSize: 11
        }
    }
}
