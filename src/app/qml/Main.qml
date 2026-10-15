// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    width: 1100
    height: 720
    minimumWidth: 760
    minimumHeight: 560
    visible: true
    title: qsTr("SynBench")

    // Temporary content until the QML foundation and System info screen
    // (tasks 12 and 17): shows the administrator gate and what was detected.
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 8

        Label {
            visible: AppController.authState === AppController.Authorising
            text: qsTr("Authorising… approve the administrator prompt to continue.")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: AppController.systemInfoReady ? AppController.systemSummary : qsTr("Detecting hardware…")
        }
        Label {
            visible: AppController.systemInfoReady && !AppController.memoryModulesKnown
            text: qsTr("Reading memory details…")
        }
        Repeater {
            model: AppController.memoryModules
            delegate: Label {
                required property string modelData
                text: modelData
            }
        }
        Item { Layout.fillHeight: true }
    }

    AuthDialog {
        visible: AppController.authState === AppController.Failed
        message: AppController.authMessage
        onRetryClicked: AppController.retryAuth()
        onQuitClicked: AppController.quit()
    }
}
