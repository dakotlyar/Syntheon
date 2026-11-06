// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Syntheon

// The one dialog style: themed panel, title (and optional subtitle) in the
// app's title font, padding. Every dialog in qml/dialogs is an SbDialog, so none
// falls back to the Basic style's white title bar in dark mode.
// Blocking by default (modal, no close on Escape or a click outside); a dialog
// that may be dismissed sets closePolicy itself.
Dialog {
    id: root

    property string subtitle

    modal: true
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: parent
    padding: 18
    background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: 8 }
    header: ColumnLayout {
        objectName: "dialogHeader"
        spacing: 2
        SbText {
            objectName: "dialogTitle"
            Layout.fillWidth: true
            text: root.title
            kind: "title"
            leftPadding: 18; topPadding: 16; rightPadding: 18
            wrapMode: Text.WordWrap
        }
        SbText {
            objectName: "dialogSubtitle"
            Layout.fillWidth: true
            visible: root.subtitle.length > 0
            text: root.subtitle
            kind: "small"
            leftPadding: 18; rightPadding: 18
            wrapMode: Text.WordWrap
        }
    }
}
