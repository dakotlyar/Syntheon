// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

// The live run: one row per planned test, the log, Stop and "View results so
// far" (§3, D3). After the run it stays as a summary until the next one.
ScreenFrame {
    id: root

    title: RunController.hasRun ? RunController.runTitle : qsTr("Progress")
    subtitle: RunController.hasRun ? RunController.runSubtitle : qsTr("Nothing is running.")

    actions: [
        SbButton {
            objectName: "stopRun"
            visible: RunController.running
            text: RunController.stopping ? qsTr("Stopping…") : qsTr("Stop run")
            danger: true
            enabled: !RunController.stopping
            onClicked: RunController.stop()
        },
        SbButton {
            objectName: "viewResults"
            visible: RunController.hasRun
            text: RunController.running ? qsTr("View results so far") : qsTr("View results")
            onClicked: { Results.openCurrent(); AppController.section = "results" }
        }
    ]

    // A hung test can't be killed (R7): say so plainly.
    Banner {
        objectName: "restartBanner"
        Layout.fillWidth: true
        tone: "bad"
        text: RunController.needsRestart
              ? qsTr("A test stopped responding, so the run was saved as interrupted. Restart SynBench to run tests again.")
              : ""
    }

    // Nothing has run yet.
    Box {
        objectName: "emptyState"
        visible: !RunController.hasRun
        Layout.fillWidth: true
        implicitHeight: emptyColumn.implicitHeight + 36
        ColumnLayout {
            id: emptyColumn
            anchors.fill: parent
            anchors.margins: 18
            spacing: 10
            SbText { text: qsTr("No run in progress"); font.weight: Font.DemiBold }
            SbText {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Pick tests on the Run tests screen and start them. Progress, a live log and the Stop button appear here.")
                kind: "small"
            }
            SbButton {
                text: qsTr("Go to Run tests")
                onClicked: AppController.section = "run"
            }
        }
    }

    // --- the queue ---
    Box {
        objectName: "queueBox"
        visible: RunController.hasRun
        Layout.fillWidth: true
        implicitHeight: queueColumn.implicitHeight

        ColumnLayout {
            id: queueColumn
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0

            Repeater {
                model: RunController.queue
                delegate: Item {
                    id: qrow
                    required property int index
                    required property string testId
                    required property string name
                    required property string colorToken
                    required property string runState
                    required property real fraction
                    required property string stage
                    required property string stateText

                    objectName: "queue-" + testId
                    Layout.fillWidth: true
                    implicitHeight: 52
                    Accessible.role: Accessible.ListItem
                    Accessible.name: name + ", " + stateText

                    Rectangle {
                        visible: qrow.index < RunController.queue.rowCount() - 1
                        anchors.bottom: parent.bottom
                        width: parent.width; height: 1; color: Theme.line
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        spacing: 12
                        Swatch {
                            implicitHeight: 26
                            color: qrow.runState === "na" || qrow.runState === "not_run" ? Theme.na : Theme.testColor(qrow.colorToken)
                        }
                        ColumnLayout {
                            Layout.preferredWidth: 170
                            Layout.maximumWidth: 220
                            spacing: 0
                            SbText { text: qrow.name; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true }
                            // The stage's name elides at its end; the "· rep N of
                            // 5 · 1:38" or "· warming up · 1:38" tail (else the
                            // last "· …" part) always shows (Zeb, rounds 2-5).
                            // Plain positioning inside the column's width: a
                            // nested RowLayout fed its text's width back into
                            // the column (C2, AP-5 L5).
                            Item {
                                id: stageRow
                                readonly property string full: qrow.stage
                                readonly property int cut: {
                                    for (const mark of [" · rep ", " · warming up"]) {
                                        const at = full.indexOf(mark)
                                        if (at >= 0)
                                            return at
                                    }
                                    return full.lastIndexOf(" · ")
                                }
                                readonly property color tone: qrow.runState === "error" || qrow.runState === "timed_out"
                                                              || qrow.runState === "interrupted" ? Theme.bad : Theme.muted
                                Layout.fillWidth: true
                                implicitWidth: 0
                                implicitHeight: stageHead.implicitHeight
                                clip: true  // never past the column (AP-5 round 4 L5)
                                FontMetrics { id: stageMetrics; font: stageHead.font }
                                SbText {
                                    id: stageHead
                                    objectName: "stage"
                                    text: stageRow.cut > 0 ? stageRow.full.slice(0, stageRow.cut) : stageRow.full
                                    kind: "small"
                                    elide: Text.ElideRight
                                    width: Math.max(0, Math.min(implicitWidth, stageRow.width - (stageTail.visible ? stageTail.width : 0)))
                                    // Too narrow for a letter and "…": nothing, not a
                                    // lone "…" (the tail alone fills the row).
                                    visible: text.length > 0 && width >= Math.min(implicitWidth, stageMetrics.advanceWidth("W…"))
                                    color: stageRow.tone
                                }
                                SbText {
                                    id: stageTail
                                    objectName: "stageTail"
                                    visible: stageRow.cut > 0
                                    x: stageHead.visible ? Math.min(stageHead.width, stageHead.paintedWidth) : 0
                                    text: stageRow.cut > 0 ? stageRow.full.slice(stageRow.cut) : ""
                                    // Wider than the whole column: it elides too.
                                    width: Math.min(implicitWidth, stageRow.width)
                                    elide: Text.ElideRight
                                    kind: "small"
                                    color: stageRow.tone
                                }
                            }
                        }
                        ProgressTrack {
                            Layout.fillWidth: true
                            value: qrow.runState === "done" ? 1 : qrow.fraction
                            fill: qrow.runState === "error" || qrow.runState === "timed_out" || qrow.runState === "interrupted"
                                  ? Theme.bad : qrow.runState === "cancelled" || qrow.runState === "not_run" || qrow.runState === "na"
                                  ? Theme.na : Theme.accent
                        }
                        SbText {
                            objectName: "state"
                            Layout.preferredWidth: 72
                            horizontalAlignment: Text.AlignRight
                            text: qrow.stateText
                            kind: "data"
                            font.pixelSize: 12
                            color: qrow.runState === "done" ? Theme.good
                                   : qrow.runState === "error" || qrow.runState === "timed_out" ? Theme.bad : Theme.ink
                        }
                    }
                }
            }
        }
    }

    // --- the live log ---
    Box {
        visible: RunController.hasRun
        Layout.fillWidth: true
        implicitHeight: logColumn.implicitHeight

        ColumnLayout {
            id: logColumn
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 12
                SbText { text: qsTr("Live log"); font.weight: Font.DemiBold; Layout.fillWidth: true }
                Pill {
                    objectName: "currentStage"
                    visible: RunController.running && RunController.currentStage.length > 0
                    text: RunController.currentStage
                    tone: "acc"
                }
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 220
                color: Theme.sunk
                radius: 8
                // Square off the top corners under the header.
                Rectangle { width: parent.width; height: 8; color: parent.color }

                ListView {
                    id: logView
                    objectName: "logView"
                    anchors.fill: parent
                    anchors.margins: 10
                    clip: true
                    model: RunController.log
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    // Follow the newest line unless the user scrolled up.
                    property bool atEnd: true
                    onMovementEnded: atEnd = atYEnd
                    onCountChanged: if (atEnd) Qt.callLater(positionViewAtEnd)
                    // Three real columns: time, the test (colour dot + name, elided to a
                    // fixed width), then the message without the test name it would
                    // otherwise repeat ("GPU  GPU: Native 2D" -> "GPU  Native 2D").
                    delegate: Item {
                        id: logLine
                        required property string time
                        required property string testName
                        required property string testColor
                        required property string body
                        required property string level
                        readonly property color tint: level === "error" ? Theme.bad : level === "warning" ? Theme.warn : Theme.ink
                        width: logView.width - 12
                        height: Math.max(lineBody.implicitHeight, lineTime.implicitHeight) + 3
                        SbText {
                            id: lineTime
                            objectName: "logTime"
                            width: 62
                            text: logLine.time
                            kind: "data"; font.pixelSize: 12
                            color: Theme.muted
                        }
                        Rectangle {
                            x: 66; y: 5
                            width: 6; height: 6; radius: 3
                            visible: logLine.testName.length > 0
                            color: Theme.testColor(logLine.testColor)
                        }
                        SbText {
                            objectName: "logTest"
                            x: 76; width: 104
                            text: logLine.testName
                            elide: Text.ElideRight
                            kind: "data"; font.pixelSize: 12
                            color: logLine.tint
                        }
                        SbText {
                            id: lineBody
                            objectName: "logBody"
                            x: 184; width: parent.width - x
                            text: logLine.body
                            textFormat: Text.PlainText
                            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                            kind: "data"; font.pixelSize: 12
                            lineHeight: 1.3
                            color: logLine.tint
                        }
                    }
                }
            }
        }
    }
}
