// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import SynBench

// A table of already-formatted cells; the first column left, the rest right in
// the data font.
Box {
    id: root

    property var block
    readonly property var columns: block ? block.columns : []
    readonly property var rows: block ? block.rows : []

    implicitHeight: content.implicitHeight
    clip: true

    readonly property var unitHeaders: ["ns", "µs", "ms", "s", "min", "fps", "GB/s", "MB/s", "IOPS", "MT/s", "Gpx/s",
                                        "Mpx/s", "MOps/s", "MFLOPS", "tok/s", "inf/s", "it/s", "W", "Wh", "%", "points"]
    // A header that is just a unit keeps its case; anything else is capitals.
    function headerText(s) { return unitHeaders.indexOf(s) >= 0 ? s : s.toUpperCase() }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        SbText {
            visible: root.block && root.block.title.length > 0
            Layout.margins: 12
            text: root.block ? root.block.title : ""
            font.weight: Font.DemiBold
        }

        // header
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: headRow.implicitHeight + 16
            color: Theme.sunk
            RowLayout {
                id: headRow
                anchors.fill: parent
                anchors.leftMargin: 12; anchors.rightMargin: 12
                spacing: 12
                Repeater {
                    model: root.columns
                    delegate: SbText {
                        required property string modelData
                        required property int index
                        Layout.fillWidth: true
                        Layout.preferredWidth: index === 0 ? 2 : 1
                        horizontalAlignment: index === 0 ? Text.AlignLeft : Text.AlignRight
                        // Capitals by hand: automatic capitalisation would turn
                        // "µs" into a Greek capital Mu and "GB/s" into "GB/S".
                        text: root.headerText(modelData)
                        kind: "label"
                        font.capitalization: Font.MixedCase
                        elide: Text.ElideRight
                    }
                }
            }
        }
        Repeater {
            model: root.rows
            delegate: Item {
                id: tableRow
                required property var modelData
                required property int index
                Layout.fillWidth: true
                implicitHeight: rowLayout.implicitHeight + 16
                Rectangle {
                    visible: tableRow.index < root.rows.length - 1
                    anchors.bottom: parent.bottom
                    width: parent.width; height: 1; color: Theme.line
                }
                RowLayout {
                    id: rowLayout
                    anchors.fill: parent
                    anchors.leftMargin: 12; anchors.rightMargin: 12
                    spacing: 12
                    Repeater {
                        model: tableRow.modelData
                        delegate: SbText {
                            required property string modelData
                            required property int index
                            Layout.fillWidth: true
                            Layout.preferredWidth: index === 0 ? 2 : 1
                            horizontalAlignment: index === 0 ? Text.AlignLeft : Text.AlignRight
                            text: modelData
                            kind: index === 0 ? "body" : "data"
                            font.pixelSize: 13
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }
}
