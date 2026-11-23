# SPDX-License-Identifier: GPL-3.0-or-later

# App-side controllers and models, in a library so they can be unit-tested.
qt_add_library(syntheon_appcore STATIC
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
qt_add_resources(syntheon_appcore "syntheon_fonts"
    PREFIX "/syntheon/fonts"
    BASE "${PROJECT_SOURCE_DIR}/assets/fonts"
    FILES
        "${PROJECT_SOURCE_DIR}/assets/fonts/Archivo-Variable.ttf"
        "${PROJECT_SOURCE_DIR}/assets/fonts/IBMPlexSans-Variable.ttf"
        "${PROJECT_SOURCE_DIR}/assets/fonts/IBMPlexMono-Regular.ttf"
        "${PROJECT_SOURCE_DIR}/assets/fonts/IBMPlexMono-Medium.ttf"
)
# THIRD-PARTY-NOTICES.txt (D11), shown by Settings > Licences.
qt_add_resources(syntheon_appcore "syntheon_notices"
    PREFIX "/syntheon"
    BASE "${PROJECT_BINARY_DIR}"
    FILES "${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt"
)
target_link_libraries(syntheon_appcore
    PUBLIC syntheon_core syntheon_storage syntheon_sysinfo syntheon_privileged syntheon_builtin_tests syntheon_power Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick
    PRIVATE syntheon_version)
target_include_directories(syntheon_appcore PUBLIC "${PROJECT_SOURCE_DIR}/src")
if(SYNTHEON_DEV_HOOKS)
    target_sources(syntheon_appcore PRIVATE screenshot_tour.h screenshot_tour.cpp)
endif()

# Theme.qml and DetailState.qml are singletons (set before the module is declared).
set_source_files_properties(qml/theme/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/detail/DetailState.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# The QML module lives in a library so the Qt Quick tests (tests/qml) can import
# it; the executable only links it and starts the app.
#
# OUTPUT_DIRECTORY ends in the module's URI, so <build>/src/app is an import path
# that resolves "import Syntheon" for the tooling (qmllint, qmlls, all_qmllint).
# The app itself loads the module from its resources, not from this folder.
#
# qml_singletons.h declares the app's C++ singletons (QML_FOREIGN) to the
# module, so the type compiler and qmllint see them; the controllers stay in
# syntheon_appcore, whose metatypes are extracted for that.
qt_extract_metatypes(syntheon_appcore)
qt_add_qml_module(syntheon_ui
    URI Syntheon
    VERSION 1.0
    STATIC
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Syntheon"
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
        qml/detail/DetailState.qml
        qml/detail/DetailPage.qml
        qml/screens/DetailScreen.qml
        qml/dialogs/LogDialog.qml
        qml/dialogs/LicencesDialog.qml
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
target_link_libraries(syntheon_ui PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::QuickShapes syntheon_appcore)

qt_add_executable(syntheon_app WIN32 main.cpp)
set_target_properties(syntheon_app PROPERTIES OUTPUT_NAME Syntheon)
target_link_libraries(syntheon_app PRIVATE
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    syntheon_ui
    syntheon_uiplugin
    syntheon_appcore
    syntheon_version
)
# The CPU test starts its encryption worker from the app's folder (§5.5).
add_dependencies(syntheon_app syntheon-cpucrypto)
# The GPU test starts its worker from the same folder (Phase 4a §2.2).
add_dependencies(syntheon_app syntheon-gpu)
if(TARGET syntheon-helper)
    add_dependencies(syntheon_app syntheon-helper)  # pkexec runs it from the same folder (dev builds)
endif()

# ---------------------------------------------------------------- Windows: manifest, version, icon
if(WIN32)
    # The exe's resources: icon and version info (file version X.Y.Z.0)
    # from the .rc, plus the manifest asking for administrator rights (P12/D1).
    set(SYNTHEON_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
    configure_file(windows/syntheon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/syntheon.rc" @ONLY)
    target_sources(syntheon_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/syntheon.rc")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/syntheon.rc" PROPERTIES
        OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/assets/syntheon.ico")
    # ONE manifest, merged and embedded by CMake's link step from this source
    # file (a second one from the .rc gave "CVT1100: duplicate resource
    # MANIFEST 1"). The linker's own default, an asInvoker execution level, is
    # switched off so it can't conflict with ours (requireAdministrator).
    target_sources(syntheon_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/windows/Syntheon.manifest")
    if(MSVC)
        target_link_options(syntheon_app PRIVATE /MANIFESTUAC:NO)
        # Libraries the exe links to are loaded before main(): from the application
        # folder and System32 only (0x200 | 0x800), never the current folder or PATH.
        # PACKAGED builds only: a development build finds Qt through PATH.
        if(NOT SYNTHEON_DEV_HOOKS)
            target_link_options(syntheon_app PRIVATE /DEPENDENTLOADFLAG:0xA00)
        endif()
    endif()
endif()

# ---------------------------------------------------------------- install tree (packaging)
# `cmake --install <build> --prefix <dir>` produces what the installers /
# AppImage pack:
#   Windows:  <dir>/Syntheon.exe + LICENSE.txt + THIRD-PARTY-NOTICES.txt +
#             the app-local VC++ and Universal CRT DLLs (P16); deploy.ps1
#             then adds Qt
#   Linux:    <dir>/bin/Syntheon, <dir>/libexec/syntheon-helper, the desktop
#             file, metainfo, icons, share/doc/syntheon/{LICENSE.txt,
#             THIRD-PARTY-NOTICES.txt}; packaging/linux/make-appdir.py then
#             bundles Qt into the AppDir
# Test-only binaries are never installed (packaging/checks/no-test-binaries.sh).
include(GNUInstallDirs)
if(WIN32)
    install(TARGETS syntheon_app RUNTIME DESTINATION .)
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION . RENAME LICENSE.txt)
    install(FILES "${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt" DESTINATION .)
    # Qt's DLLs, plugins and QML imports are added by packaging/windows/deploy.ps1
    # (windeployqt; for ARM64 it runs the host tool with --qtpaths of the target
    # Qt, which is Qt's documented cross route), not by an install(SCRIPT), so
    # the native and cross builds deploy the same way.
    # App-local CRT: neither installer needs a VC++ redistributable step.
    # CI hands in the redist folders for the TARGET architecture
    # (packaging/windows/find-crt.ps1), which is the only reliable way to get
    # the ARM64 ones in a cross build.
    set(SYNTHEON_CRT_DIRS "" CACHE STRING "Folders with the target architecture's VC++ runtime and UCRT DLLs to ship app-local")
    # The folders arrive with backslashes (C:\Program Files\...); written as they
    # are into the generated install code, "\P" is an invalid escape (CMake dev
    # warnings, and the file name is mangled). Forward slashes, and quoted.
    if(NOT SYNTHEON_DEV_HOOKS AND NOT SYNTHEON_CRT_DIRS)
        message(FATAL_ERROR "A package build needs -DSYNTHEON_CRT_DIRS=<folder with the target's VC++ runtime> "
                            "(packaging/windows/find-crt.ps1)")
    endif()
    file(TO_CMAKE_PATH "${SYNTHEON_CRT_DIRS}" _syntheon_crt_dirs)
    foreach(_dir IN LISTS _syntheon_crt_dirs)
        file(GLOB _crt_dlls "${_dir}/*.dll")
        if(NOT _crt_dlls)
            if(SYNTHEON_DEV_HOOKS)
                message(WARNING "SYNTHEON_CRT_DIRS: no DLLs in '${_dir}'")
            else()
                message(FATAL_ERROR "SYNTHEON_CRT_DIRS: no DLLs in '${_dir}' (a package build must ship the C++ runtime)")
            endif()
        endif()
        foreach(_dll IN LISTS _crt_dlls)
            install(FILES "${_dll}" DESTINATION .)
        endforeach()
    endforeach()
else()
    # The AppDir keeps libraries in usr/lib next to usr/bin.
    set_target_properties(syntheon_app PROPERTIES INSTALL_RPATH "\$ORIGIN/../lib")
    install(TARGETS syntheon_app RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    # The licence and the third-party notices (D11), in the AppImage too.
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/syntheon" RENAME LICENSE.txt)
    install(FILES "${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/syntheon")
    if(TARGET syntheon-helper)
        install(TARGETS syntheon-helper RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
    endif()
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/io.github.dakotlyar.Syntheon.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    # The release entry comes from the VERSION file and the build date (honours
    # SOURCE_DATE_EPOCH), not from a number typed into the file.
    string(TIMESTAMP SYNTHEON_RELEASE_DATE "%Y-%m-%d" UTC)
    configure_file("${PROJECT_SOURCE_DIR}/packaging/linux/io.github.dakotlyar.Syntheon.metainfo.xml.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/io.github.dakotlyar.Syntheon.metainfo.xml" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/io.github.dakotlyar.Syntheon.metainfo.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
    install(FILES "${PROJECT_SOURCE_DIR}/assets/syntheon.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" RENAME syntheon.svg)
    install(FILES "${PROJECT_SOURCE_DIR}/assets/syntheon.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps" RENAME syntheon.png)
endif()
