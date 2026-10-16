// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// The common screen layout: title, a muted subtitle, optional actions on the
// right, and scrolling content below.
Item {
    id: root

    property string title
    property string subtitle
    property alias actions: actionRow.data
    default property alias content: body.data
    property alias headerExtra: extra.data

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + 40
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: column
            x: 20
            y: 18
            width: flick.width - 40
            spacing: 16

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    ColumnLayout { id: extra; spacing: 6; Layout.fillWidth: true }
                    SbText { text: root.title; kind: "title"; Accessible.role: Accessible.Heading }
                    SbText {
                        visible: root.subtitle.length > 0
                        Layout.fillWidth: true
                        text: root.subtitle
                        kind: "small"
                        wrapMode: Text.WordWrap
                    }
                }
                RowLayout { id: actionRow; spacing: 6 }
            }
            ColumnLayout { id: body; Layout.fillWidth: true; spacing: 16 }
        }
    }
}
