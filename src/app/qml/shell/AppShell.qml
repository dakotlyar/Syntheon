// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Syntheon

// Navigation plus the persistent content. Only the nav Loader swaps when the
// layout changes (rail <-> top tabs), so the StackLayout, and with it every
// screen's state, is never touched (§7). Below 720 px wide the rail becomes
// the top strip automatically.
//
// Children go in `content`, one per section, in the order of `sections`.
Item {
    id: root

    property string layout: "rail"   // "rail" | "tabs"
    property string section: "run"
    property string version
    signal navigate(string id)
    default property alias content: stack.data

    readonly property var sections: [
        { id: "run", label: qsTr("Run tests"), icon: "M4 2.5v11l9-5.5-9-5.5z" },
        { id: "progress", label: qsTr("Progress"),
          icon: "M2.5 8a5.5 5.5 0 1 0 11 0a5.5 5.5 0 1 0-11 0zM8 4.5V8l2.5 1.5" },
        { id: "results", label: qsTr("Results"), icon: "M2.5 13.5h11M4.5 11V7M8 11V3.5M11.5 11V6" },
        { id: "history", label: qsTr("History"), icon: "M2.5 8a5.5 5.5 0 1 0 1.6-3.9M2.5 2.5v2.5H5" },
        { id: "compare", label: qsTr("Compare"), icon: "M5 2.5v11M11 2.5v11M2.5 6H5M11 10h2.5" },
        { id: "system", label: qsTr("System info"),
          icon: "M5 4h6a1 1 0 0 1 1 1v6a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1zM6 1.5V4M10 1.5V4M6 12v2.5M10 12v2.5M1.5 6H4M1.5 10H4M12 6h2.5M12 10h2.5" },
        { spacer: true },
        { id: "settings", label: qsTr("Settings"),
          icon: "M6 8a2 2 0 1 0 4 0a2 2 0 1 0-4 0zM8 1.5v2M8 12.5v2M1.5 8h2M12.5 8h2M3.4 3.4l1.4 1.4M11.2 11.2l1.4 1.4M3.4 12.6l1.4-1.4M11.2 4.8l1.4-1.4" }
    ]
    readonly property bool narrow: width < 720
    readonly property bool useRail: layout !== "tabs" && !narrow
    readonly property int currentIndex: {
        var n = 0
        for (let i = 0; i < sections.length; ++i) {
            if (sections[i].spacer) continue
            if (sections[i].id === section) return n
            ++n
        }
        return 0
    }

    Component {
        id: railComponent
        RailNav {
            sections: root.sections
            current: root.section
            version: root.version
            onNavigate: id => root.navigate(id)
        }
    }
    Component {
        id: tabsComponent
        TopTabsNav {
            sections: root.sections
            current: root.section
            version: root.version
            onNavigate: id => root.navigate(id)
        }
    }

    GridLayout {
        anchors.fill: parent
        columns: root.useRail ? 2 : 1
        rowSpacing: 0
        columnSpacing: 0

        Loader {
            id: nav
            objectName: "navLoader"
            Layout.fillHeight: root.useRail
            Layout.fillWidth: !root.useRail
            sourceComponent: root.useRail ? railComponent : tabsComponent
        }
        StackLayout {
            id: stack
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.currentIndex
        }
    }
}
