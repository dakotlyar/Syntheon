// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import SynBench

// KPI cards: label, value, unit (the mockup's .kpis).
GridLayout {
    id: root

    property var block
    readonly property var items: block ? block.items : []

    columns: Math.max(1, Math.min(items.length, Math.floor((width + 10) / 160)))
    columnSpacing: 10
    rowSpacing: 10

    Repeater {
        model: root.items
        delegate: Box {
            id: kpi
            required property var modelData
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            implicitHeight: col.implicitHeight + 20
            Accessible.role: Accessible.StaticText
            Accessible.name: modelData.label + " " + modelData.value + " " + modelData.unit

            ColumnLayout {
                id: col
                anchors.fill: parent
                anchors.margins: 10
                anchors.leftMargin: 12
                spacing: 2
                SbText { text: kpi.modelData.label; kind: "label"; elide: Text.ElideRight; Layout.fillWidth: true }
                SbText {
                    objectName: "kpiValue"
                    text: kpi.modelData.value
                    kind: "data"
                    font.pixelSize: 20
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                SbText { visible: text.length > 0; text: kpi.modelData.unit; kind: "small"; elide: Text.ElideRight; Layout.fillWidth: true }
            }
        }
    }
}
