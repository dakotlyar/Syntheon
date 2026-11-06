// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Syntheon

// The "not available" page: a pill, the headline, why. Every N/A test's
// Detail page is this block (withNotAvailableState in core/detail.cpp).
Box {
    id: root
    objectName: "naStateBlock"

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
