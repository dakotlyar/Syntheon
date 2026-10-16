// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import SynBench

// The "not available" page (NPU): a pill, the headline, why.
Box {
    id: root

    property var block

    implicitHeight: col.implicitHeight + 44

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: 22
        spacing: 8
        Pill { text: qsTr("N/A"); tone: "na" }
        SbText { text: root.block ? root.block.title : ""; font.weight: Font.DemiBold }
        SbText {
            Layout.fillWidth: true
            text: root.block ? root.block.body : ""
            kind: "muted"
            wrapMode: Text.WordWrap
        }
    }
}
