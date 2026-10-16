// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import SynBench

ApplicationWindow {
    id: window

    width: 1100
    height: 720
    minimumWidth: 760
    minimumHeight: 560
    visible: true
    title: qsTr("SynBench")
    color: Theme.win

    // The theme is set only in Settings; "system" follows the OS live.
    Binding { target: Theme; property: "mode"; value: Settings.theme }

    // D18: native OS title bar + this strip inside the window.
    header: HeaderStrip {
        machine: AppController.headerMachine
        detail: AppController.headerDetail
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Banner {
            objectName: "emulationBanner"
            Layout.fillWidth: true
            text: AppController.emulationText
        }

        AppShell {
            Layout.fillWidth: true
            Layout.fillHeight: true
            layout: Settings.layout
            section: AppController.section
            version: AppController.version
            onNavigate: id => AppController.section = id

            RunScreen {}
            ProgressScreen {}
            ResultsScreen {}
            HistoryScreen {}
            CompareScreen {}
            SystemInfoScreen {}
            SettingsScreen {}
        }
    }

    // Launch gates, in order: the history must open, then administrator
    // access. Both block; neither has a limited mode.
    StorageProblemDialog {
        visible: StorageController.startupKind !== "none"
        newerVersion: StorageController.startupKind === "newer"
        message: StorageController.startupMessage
        onUseDefault: error = StorageController.useDefaultLocation()
        onChooseFolder: path => error = StorageController.openHistoryIn(path)
        onQuitClicked: AppController.quit()
    }
    AuthDialog {
        visible: StorageController.startupKind === "none" && AppController.authStateName !== "ready"
        authorising: AppController.authStateName === "authorising"
        message: AppController.authMessage
        onRetryClicked: AppController.retryAuth()
        onQuitClicked: AppController.quit()
    }
}
