// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Syntheon

// The run summary: the overall dial, one score row per test, the run's status
// (Stopped / Interrupted / Recovered, with a "partial" marker on the overall,
// N2), "View log" (D16). A row opens that test's Detail page, which lives under
// Results (the rail keeps Results highlighted).
Item {
    id: root

    readonly property bool showingDetail: Results.detailOpen

    function openDetail(testId) { Results.showDetail(testId) }

    // Showing the page follows this session's run, unless a run was opened on
    // purpose from History (pinned); a new run un-pins.
    onVisibleChanged: if (visible && AppController.section === "results" && !showingDetail && RunController.hasRun
                          && !Results.pinned && (!Results.valid || Results.uuid !== RunController.runUuid))
        Results.openCurrent()

    StackLayout {
        anchors.fill: parent
        currentIndex: root.showingDetail ? 1 : 0

        ScreenFrame {
            id: summary
            title: Results.title
            subtitle: Results.subtitle

            actions: [
                Pill {
                    objectName: "statusBadge"
                    visible: Results.statusLabel.length > 0
                    text: Results.statusLabel
                    tone: Results.statusKey === "recovered" ? "good" : "warn"
                },
                SbButton {
                    objectName: "viewLog"
                    visible: Results.valid
                    text: qsTr("View log")
                    onClicked: logDialog.openLog()
                }
            ]

            Box {
                objectName: "emptyState"
                visible: !Results.valid
                Layout.fillWidth: true
                implicitHeight: emptyCol.implicitHeight + 36
                ColumnLayout {
                    id: emptyCol
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 10
                    SbText { text: qsTr("No results yet"); font.weight: Font.DemiBold }
                    SbText {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Run some tests and their results appear here. Every run is saved in History.")
                        kind: "small"
                    }
                    SbButton { text: qsTr("Go to Run tests"); onClicked: AppController.section = "run" }
                }
            }

            GridLayout {
                visible: Results.valid
                Layout.fillWidth: true
                columns: summary.width < 860 ? 1 : 2
                columnSpacing: 16
                rowSpacing: 16

                // --- the overall dial ---
                Box {
                    objectName: "overallBox"
                    Layout.preferredWidth: 240
                    Layout.alignment: Qt.AlignTop
                    Layout.fillWidth: summary.width < 860
                    implicitHeight: overallCol.implicitHeight + 36

                    ColumnLayout {
                        id: overallCol
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 2
                        ScoreDial {
                            Layout.alignment: Qt.AlignHCenter
                            fraction: Results.scoredFraction
                        }
                        SbText {
                            objectName: "overallScore"
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: -38
                            text: Results.overallText
                            kind: "display"
                            font.family: Theme.dataFamily
                            font.variableAxes: ({})
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 6
                            SbText { text: qsTr("Overall score"); kind: "label" }
                            Pill {
                                objectName: "partialMarker"
                                visible: Results.overallPartial
                                text: qsTr("partial")
                                tone: "warn"
                            }
                        }
                        SbText {
                            objectName: "overallNote"
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: Results.overallNote
                            kind: "small"
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                // --- one row per test ---
                Box {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: rowsCol.implicitHeight

                    ColumnLayout {
                        id: rowsCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 0

                        Repeater {
                            model: Results.rows
                            delegate: Item {
                                id: srow
                                required property var modelData
                                required property int index
                                objectName: "result-" + modelData.testId
                                Layout.fillWidth: true
                                implicitHeight: 52
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: qsTr("Open %1 details").arg(modelData.name)
                                Keys.onReturnPressed: root.openDetail(modelData.testId)
                                Keys.onSpacePressed: root.openDetail(modelData.testId)

                                Rectangle {
                                    anchors.fill: parent
                                    color: hover.hovered ? Theme.sunk : "transparent"
                                    border.color: Theme.accent
                                    border.width: srow.activeFocus ? 2 : 0
                                }
                                Rectangle {
                                    visible: srow.index < Results.rows.length - 1
                                    anchors.bottom: parent.bottom
                                    width: parent.width; height: 1; color: Theme.line
                                }
                                HoverHandler { id: hover }
                                TapHandler { onTapped: root.openDetail(srow.modelData.testId) }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 14
                                    anchors.rightMargin: 14
                                    spacing: 12
                                    Swatch {
                                        implicitHeight: 30
                                        color: srow.modelData.na ? Theme.na : Theme.testColor(srow.modelData.colorToken)
                                    }
                                    SbText {
                                        Layout.preferredWidth: 110
                                        text: srow.modelData.name
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                        color: srow.modelData.na ? Theme.na : Theme.ink
                                    }
                                    SbText {
                                        objectName: "score"
                                        Layout.preferredWidth: 72
                                        horizontalAlignment: Text.AlignRight
                                        text: srow.modelData.score
                                        kind: "data"
                                        font.pixelSize: 18
                                        color: srow.modelData.na ? Theme.na : Theme.ink
                                    }
                                    SbText {
                                        Layout.fillWidth: true
                                        text: srow.modelData.raw
                                        kind: "data"
                                        font.pixelSize: 12
                                        color: Theme.muted
                                        elide: Text.ElideRight
                                    }
                                    SbText { text: "›"; color: Theme.muted }
                                }
                            }
                        }
                    }
                }
            }
            SbText {
                visible: Results.valid
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("There is no JSON export button here yet. JSON output arrives with the CLI in Phase 9.")
                kind: "small"
            }
        }

        DetailScreen {
            onBack: Results.closeDetail()
        }
    }

    LogDialog { id: logDialog; objectName: "logDialog" }
}
