# SPDX-License-Identifier: GPL-3.0-or-later

# App-side controllers and models, in a library so they can be unit-tested.
qt_add_library(synbench_appcore STATIC
    app_runtime.h app_runtime.cpp
    fonts.h fonts.cpp
    controllers/app_controller.h controllers/app_controller.cpp
    controllers/settings_controller.h controllers/settings_controller.cpp
    controllers/storage_controller.h controllers/storage_controller.cpp
    controllers/run_controller.h controllers/run_controller.cpp
    controllers/results_controller.h controllers/results_controller.cpp
    controllers/compare_controller.h controllers/compare_controller.cpp
    controllers/startup_controller.h controllers/startup_controller.cpp
    single_instance.h single_instance.cpp
    process_hardening.h process_hardening.cpp
    self_check.h self_check.cpp
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
    PUBLIC synbench_core synbench_storage synbench_sysinfo synbench_privileged synbench_builtin_tests synbench_power Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick
    PRIVATE synbench_version)
target_include_directories(synbench_appcore PUBLIC "${PROJECT_SOURCE_DIR}/src")
if(SYNBENCH_DEV_HOOKS)
    target_sources(synbench_appcore PRIVATE screenshot_tour.h screenshot_tour.cpp)
endif()

# Theme.qml is a singleton (set before the module is declared).
set_source_files_properties(qml/theme/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# The QML module lives in a library so the Qt Quick tests (tests/qml) can import
# it; the executable only links it and starts the app.
#
# OUTPUT_DIRECTORY ends in the module's URI, so <build>/src/app is an import path
# that resolves "import SynBench" for the tooling (qmllint, qmlls, all_qmllint).
# The app itself loads the module from its resources, not from this folder.
#
# qml_singletons.h declares the app's C++ singletons (QML_FOREIGN) to the
# module, so the type compiler and qmllint see them; the controllers stay in
# synbench_appcore, whose metatypes are extracted for that.
qt_extract_metatypes(synbench_appcore)
qt_add_qml_module(synbench_ui
    URI SynBench
    VERSION 1.0
    STATIC
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/SynBench"
    SOURCES
        qml_singletons.h
    QML_FILES
        qml/Main.qml
        qml/theme/Theme.qml
        qml/components/SbText.qml
        qml/components/Box.qml
        qml/components/Pill.qml
        qml/components/SbDialog.qml
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
        qml/dialogs/FirstRunDialog.qml
        qml/dialogs/InUseDialog.qml
        qml/dialogs/RecoveryNoticeDialog.qml
        qml/dialogs/StorageProblemDialog.qml
)
target_link_libraries(synbench_ui PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::QuickShapes synbench_appcore)

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
# The CPU test starts its encryption worker from the app's folder (§5.5).
add_dependencies(synbench_app synbench-cpucrypto)
if(TARGET synbench-helper)
    add_dependencies(synbench_app synbench-helper)  # pkexec runs it from the same folder (dev builds)
endif()

# ---------------------------------------------------------------- Windows: manifest, version, icon
if(WIN32)
    # The exe's resources: icon and version info (file version 2026.11.N.0)
    # from the .rc, plus the manifest asking for administrator rights (P12/D1).
    set(SYNBENCH_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
    configure_file(windows/synbench.rc.in "${CMAKE_CURRENT_BINARY_DIR}/synbench.rc" @ONLY)
    target_sources(synbench_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/synbench.rc")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/synbench.rc" PROPERTIES
        OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/assets/synbench.ico")
    # ONE manifest, merged and embedded by CMake's link step from this source
    # file (a second one from the .rc gave "CVT1100: duplicate resource
    # MANIFEST 1"). The linker's own default, an asInvoker execution level, is
    # switched off so it can't conflict with ours (requireAdministrator).
    target_sources(synbench_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/windows/SynBench.manifest")
    if(MSVC)
        target_link_options(synbench_app PRIVATE /MANIFESTUAC:NO)
        # Libraries the exe links to are loaded before main(): from the application
        # folder and System32 only (0x200 | 0x800), never the current folder or PATH.
        # PACKAGED builds only: a development build finds Qt through PATH.
        if(NOT SYNBENCH_DEV_HOOKS)
            target_link_options(synbench_app PRIVATE /DEPENDENTLOADFLAG:0xA00)
        endif()
    endif()
endif()

# ---------------------------------------------------------------- install tree (packaging)
# `cmake --install <build> --prefix <dir>` produces what the installers /
# AppImage pack:
#   Windows:  <dir>/SynBench.exe + LICENSE.txt + the app-local VC++ and
#             Universal CRT DLLs (P16); deploy.ps1 then adds Qt
#   Linux:    <dir>/bin/SynBench, <dir>/libexec/synbench-helper, the desktop
#             file, metainfo and icons; packaging/linux/make-appdir.py then
#             bundles Qt into the AppDir
# Test-only binaries are never installed (packaging/checks/no-test-binaries.sh).
include(GNUInstallDirs)
if(WIN32)
    install(TARGETS synbench_app RUNTIME DESTINATION .)
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION . RENAME LICENSE.txt)
    # Qt's DLLs, plugins and QML imports are added by packaging/windows/deploy.ps1
    # (windeployqt; for ARM64 it runs the host tool with --qtpaths of the target
    # Qt, which is Qt's documented cross route), not by an install(SCRIPT), so
    # the native and cross builds deploy the same way.
    # App-local CRT: neither installer needs a VC++ redistributable step.
    # CI hands in the redist folders for the TARGET architecture
    # (packaging/windows/find-crt.ps1), which is the only reliable way to get
    # the ARM64 ones in a cross build.
    set(SYNBENCH_CRT_DIRS "" CACHE STRING "Folders with the target architecture's VC++ runtime and UCRT DLLs to ship app-local")
    # The folders arrive with backslashes (C:\Program Files\...); written as they
    # are into the generated install code, "\P" is an invalid escape (CMake dev
    # warnings, and the file name is mangled). Forward slashes, and quoted.
    if(NOT SYNBENCH_DEV_HOOKS AND NOT SYNBENCH_CRT_DIRS)
        message(FATAL_ERROR "A package build needs -DSYNBENCH_CRT_DIRS=<folder with the target's VC++ runtime> "
                            "(packaging/windows/find-crt.ps1)")
    endif()
    file(TO_CMAKE_PATH "${SYNBENCH_CRT_DIRS}" _synbench_crt_dirs)
    foreach(_dir IN LISTS _synbench_crt_dirs)
        file(GLOB _crt_dlls "${_dir}/*.dll")
        if(NOT _crt_dlls)
            if(SYNBENCH_DEV_HOOKS)
                message(WARNING "SYNBENCH_CRT_DIRS: no DLLs in '${_dir}'")
            else()
                message(FATAL_ERROR "SYNBENCH_CRT_DIRS: no DLLs in '${_dir}' (a package build must ship the C++ runtime)")
            endif()
        endif()
        foreach(_dll IN LISTS _crt_dlls)
            install(FILES "${_dll}" DESTINATION .)
        endforeach()
    endforeach()
else()
    # The AppDir keeps libraries in usr/lib next to usr/bin.
    set_target_properties(synbench_app PROPERTIES INSTALL_RPATH "\$ORIGIN/../lib")
    install(TARGETS synbench_app RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    if(TARGET synbench-helper)
        install(TARGETS synbench-helper RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
    endif()
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/io.github.dakotlyar.SynBench.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    # The release entry comes from the VERSION file and the build date (honours
    # SOURCE_DATE_EPOCH), not from a number typed into the file.
    string(TIMESTAMP SYNBENCH_RELEASE_DATE "%Y-%m-%d" UTC)
    configure_file("${PROJECT_SOURCE_DIR}/packaging/linux/io.github.dakotlyar.SynBench.metainfo.xml.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/io.github.dakotlyar.SynBench.metainfo.xml" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/io.github.dakotlyar.SynBench.metainfo.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
    install(FILES "${PROJECT_SOURCE_DIR}/assets/synbench.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" RENAME synbench.svg)
    install(FILES "${PROJECT_SOURCE_DIR}/assets/synbench.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps" RENAME synbench.png)
endif()
