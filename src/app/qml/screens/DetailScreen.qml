// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// One test's detail page inside Results (the rail keeps Results highlighted).
// Tabs: the tests of this run plus, if the run had no Battery, a "Battery" tab
// with the newest Battery run on or before this one (D4, N4).
ScreenFrame {
    id: root

    signal back()

    title: Results.detailTitle
    subtitle: Results.detailSubtitle

    headerExtra: [
        SbButton {
            objectName: "backToResults"
            text: qsTr("← Results")
            onClicked: root.back()
        }
    ]

    // Detail tabs.
    Flow {
        objectName: "detailTabs"
        Layout.fillWidth: true
        spacing: 6
        Repeater {
            model: Results.detailTabs
            delegate: AbstractButton {
                id: tab
                required property var modelData
                readonly property bool active: modelData.id === Results.detailTestId
                objectName: "dtab-" + modelData.id
                text: modelData.name
                implicitHeight: 30
                leftPadding: 11; rightPadding: 11
                activeFocusOnTab: true
                Accessible.role: Accessible.PageTab
                Accessible.name: text + (modelData.separate ? qsTr(", from a separate run") : "")
                Accessible.selected: active
                onClicked: Results.selectDetail(modelData.id)
                background: Rectangle {
                    radius: 15
                    color: tab.active ? Theme.accentSoft : "transparent"
                    border.color: tab.active ? Theme.accent : Theme.line
                }
                contentItem: RowLayout {
                    spacing: 7
                    Rectangle {
                        implicitWidth: 8; implicitHeight: 8; radius: 4
                        color: tab.modelData.na ? Theme.na : Theme.testColor(tab.modelData.colorToken)
                    }
                    SbText {
                        text: tab.text
                        kind: "small"
                        font.weight: Font.DemiBold
                        color: tab.active ? Theme.ink : Theme.muted
                    }
                }
            }
        }
    }

    // "From a separate run · Mixed profile · 13 Oct 2026" (the Battery tab).
    Pill {
        objectName: "separateRunNote"
        visible: Results.detailNote.length > 0
        text: Results.detailNote
        tone: "warn"
    }

    DetailPage {
        objectName: "detailPage"
        Layout.fillWidth: true
        blocks: Results.detailBlocks
        colorToken: Results.detailColorToken
    }
}
