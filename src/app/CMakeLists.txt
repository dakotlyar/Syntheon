# SPDX-License-Identifier: GPL-3.0-or-later

# App-side controllers and models, in a library so they can be unit-tested.
qt_add_library(synbench_appcore STATIC
    app_runtime.h app_runtime.cpp
    fonts.h fonts.cpp
    screenshot_tour.h screenshot_tour.cpp
    controllers/app_controller.h controllers/app_controller.cpp
    controllers/settings_controller.h controllers/settings_controller.cpp
    controllers/storage_controller.h controllers/storage_controller.cpp
    controllers/run_controller.h controllers/run_controller.cpp
    controllers/results_controller.h controllers/results_controller.cpp
    controllers/compare_controller.h controllers/compare_controller.cpp
    models/history_model.h models/history_model.cpp
    models/detail_blocks.h models/detail_blocks.cpp
    models/log_filter_model.h models/log_filter_model.cpp
    models/options_model.h models/options_model.cpp
    models/log_model.h models/log_model.cpp
    models/run_queue_model.h models/run_queue_model.cpp
)
qt_add_resources(synbench_appcore "synbench_fonts"
    PREFIX "/synbench/fonts"
    BASE "${PROJECT_SOURCE_DIR}/assets/fonts"
    FILES
        "${PROJECT_SOURCE_DIR}/assets/fonts/Archivo-Variable.ttf"
        "${PROJECT_SOURCE_DIR}/assets/fonts/IBMPlexSans-Variable.ttf"
        "${PROJECT_SOURCE_DIR}/assets/fonts/IBMPlexMono-Regular.ttf"
        "${PROJECT_SOURCE_DIR}/assets/fonts/IBMPlexMono-Medium.ttf"
)
target_link_libraries(synbench_appcore
    PUBLIC synbench_core synbench_storage synbench_sysinfo synbench_privileged synbench_builtin_tests synbench_power Qt6::Gui Qt6::Qml Qt6::Quick
    PRIVATE synbench_version)
target_include_directories(synbench_appcore PUBLIC "${PROJECT_SOURCE_DIR}/src")

# Theme.qml is a singleton (set before the module is declared).
set_source_files_properties(qml/theme/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# The QML module lives in a library so the Qt Quick tests (tests/qml) can import
# it; the executable only links it and starts the app.
qt_add_qml_module(synbench_ui
    URI SynBench
    VERSION 1.0
    STATIC
    QML_FILES
        qml/Main.qml
        qml/theme/Theme.qml
        qml/components/SbText.qml
        qml/components/Box.qml
        qml/components/Pill.qml
        qml/components/Swatch.qml
        qml/components/ProgressTrack.qml
        qml/components/Icon.qml
        qml/components/SbButton.qml
        qml/components/SbCheck.qml
        qml/components/Segmented.qml
        qml/components/ScoreDial.qml
        qml/components/LineChart.qml
        qml/components/ScreenFrame.qml
        qml/components/SbCombo.qml
        qml/components/SbSpin.qml
        qml/components/SbField.qml
        qml/options/OptionEditor.qml
        qml/options/OptionTab.qml
        qml/shell/AppShell.qml
        qml/shell/RailNav.qml
        qml/shell/TopTabsNav.qml
        qml/shell/HeaderStrip.qml
        qml/shell/Banner.qml
        qml/detail/KpisBlock.qml
        qml/detail/TableBlock.qml
        qml/detail/GridBlock.qml
        qml/detail/ChartBlock.qml
        qml/detail/NoteBlock.qml
        qml/detail/NaStateBlock.qml
        qml/detail/DetailPage.qml
        qml/screens/DetailScreen.qml
        qml/dialogs/LogDialog.qml
        qml/screens/RunScreen.qml
        qml/screens/ProgressScreen.qml
        qml/screens/ResultsScreen.qml
        qml/screens/HistoryScreen.qml
        qml/screens/CompareScreen.qml
        qml/screens/SystemInfoScreen.qml
        qml/screens/SettingsScreen.qml
        qml/dialogs/AuthDialog.qml
        qml/dialogs/StorageProblemDialog.qml
)
target_link_libraries(synbench_ui PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::QuickShapes)

qt_add_executable(synbench_app WIN32 main.cpp)
set_target_properties(synbench_app PROPERTIES OUTPUT_NAME SynBench)
target_link_libraries(synbench_app PRIVATE
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    synbench_ui
    synbench_uiplugin
    synbench_appcore
    synbench_version
)
if(TARGET synbench-helper)
    add_dependencies(synbench_app synbench-helper)  # pkexec runs it from the same folder (dev builds)
endif()
