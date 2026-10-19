// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import SynBench

Box {
    id: root

    property var block
    property string colorToken

    implicitHeight: col.implicitHeight

    ColumnLayout {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0
        SbText {
            visible: root.block && root.block.title.length > 0
            Layout.margins: 12
            text: root.block ? root.block.title : ""
            font.weight: Font.DemiBold
        }
        Rectangle { visible: root.block && root.block.title.length > 0; Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
        LineChart {
            objectName: "chart"
            Layout.fillWidth: true
            Layout.margins: 12
            implicitHeight: 240
            points: root.block ? root.block.points : []
            logX: root.block ? root.block.logX : false
            xTicks: root.block ? root.block.xTicks : []
            yUnit: root.block ? root.block.yUnit : ""
            annotations: root.block ? root.block.annotations : []
            logY: root.block && root.block.logY !== undefined ? root.block.logY : false
            yTicks: root.block && root.block.yTicks !== undefined ? root.block.yTicks : []
            markers: root.block && root.block.markers !== undefined ? root.block.markers : []
            color: Theme.testColor(root.colorToken)
            description: root.block ? root.block.title : ""
        }
    }
}
