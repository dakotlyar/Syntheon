// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Shapes
import SynBench

// The overall-score half dial. There is no reference machine, so the arc does
// not mean "good or bad": it shows how many of the run's tests produced a
// score (`fraction`).
Item {
    id: root

    property real fraction: 0
    property color fill: Theme.accent

    implicitWidth: 170
    implicitHeight: 104
    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Overall score dial")

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: Theme.sunk
            strokeWidth: 12
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc { centerX: 85; centerY: 95; radiusX: 70; radiusY: 70; startAngle: 180; sweepAngle: 180 }
        }
        ShapePath {
            strokeColor: root.fill
            strokeWidth: 12
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: 85; centerY: 95; radiusX: 70; radiusY: 70
                startAngle: 180
                sweepAngle: 180 * Math.max(0.001, Math.min(1, root.fraction))
            }
        }
    }
}
