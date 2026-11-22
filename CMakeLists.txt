# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SyntheonVersion)
syntheon_read_version("${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(Syntheon
    VERSION ${SYNTHEON_VERSION}
    DESCRIPTION "Synthetic system benchmark"
    LANGUAGES C CXX)

option(SYNTHEON_BUILD_TESTS "Build the unit tests" ON)
# Developer hooks: the SYNTHEON_SMOKE / SYNTHEON_TOUR* / SYNTHEON_STUB_SCALE /
# SYNTHEON_DATA_DIR / SYNTHEON_SETTINGS_FILE / SYNTHEON_NO_ELEVATION
# environment variables, the screenshot tour and the test-only AppController
# hooks. ON for the dev and CI presets, OFF for packaging presets (what ships
# never reads these variables). The unit tests need them, so tests imply ON.
option(SYNTHEON_DEV_HOOKS "Compile in the developer hooks (env variables, screenshot tour, test hooks)" ON)
if(SYNTHEON_BUILD_TESTS AND NOT SYNTHEON_DEV_HOOKS)
    message(FATAL_ERROR "SYNTHEON_BUILD_TESTS needs SYNTHEON_DEV_HOOKS=ON (packaging presets build neither)")
endif()
if(SYNTHEON_DEV_HOOKS)
    add_compile_definitions(SYNTHEON_DEV_HOOKS=1)
endif()
option(SYNTHEON_RELEASE_BUILD "Release build: no -dev+<sha> suffix on the version" OFF)
set(SYNTHEON_QT_VERSION "6.12.0" CACHE STRING "Qt version the build is pinned to (set by CMakePresets.json)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Executables go to <build>/bin so the QML module folder "Syntheon" next to the
# app's sources can't collide with the "Syntheon" executable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# Shared libraries too (no shared lib remains since the GenAI C-API rewrite
# dropped the shim module; the variable is kept — harmless, avoids churn — so
# a future shared library still lands in <build>/bin rather than its source
# dir).
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(CompilerFlags)
# After the compiler flags: the third-party libraries get the same baseline.
include(ThirdParty)
include(GpuThirdParty)
include(OpenSSL)
# The Storage test's async I/O engine detection + notices (§4.1, D7), before
# the third-party notices are written.
include(StorageIo)

set(_syntheon_qt_components Core Gui Network Qml Quick QuickControls2 QuickShapes Sql)
if(SYNTHEON_BUILD_TESTS)
    list(APPEND _syntheon_qt_components QuickTest)  # tests only: never packaged
endif()
find_package(Qt6 6.12 REQUIRED COMPONENTS ${_syntheon_qt_components})
if(NOT Qt6_VERSION VERSION_EQUAL SYNTHEON_QT_VERSION)
    message(WARNING "Found Qt ${Qt6_VERSION}, but the build is pinned to Qt ${SYNTHEON_QT_VERSION}")
endif()
qt_standard_project_setup(REQUIRES 6.12)

syntheon_generate_version_header()

# THIRD-PARTY-NOTICES.txt: the libraries above (cmake/ThirdParty.cmake), Qt
# (LGPL-3.0, linked dynamically) and the bundled fonts (OFL-1.1). Written
# before src/, which embeds it in the app (Settings > Licences) and installs
# it beside LICENSE.txt.
syntheon_add_notice(NAME "Qt" VERSION ${Qt6_VERSION} LICENCE "LGPL-3.0-only"
    FILE "${PROJECT_SOURCE_DIR}/packaging/licenses/LGPL-3.0.txt"
    NOTE "Syntheon links the Qt libraries dynamically: you may replace them with your own build.\nQt's source code: https://download.qt.io/official_releases/qt/ and https://code.qt.io\nThe LGPL-3.0 adds to the GPL-3.0, whose text is in LICENSE.txt.")
syntheon_add_notice(NAME "Archivo font" LICENCE "OFL-1.1" FILE "${PROJECT_SOURCE_DIR}/assets/fonts/OFL-Archivo.txt")
syntheon_add_notice(NAME "IBM Plex Sans and IBM Plex Mono fonts" LICENCE "OFL-1.1"
    FILE "${PROJECT_SOURCE_DIR}/assets/fonts/OFL-IBMPlex.txt")
# The code bundled inside Qt's deployment, and ICU on Linux (AP-5 M3).
include(QtNotices)
syntheon_add_qt_deployment_notices()
syntheon_write_third_party_notices()

add_subdirectory(src)


if(SYNTHEON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tools/cpu-golden)
    add_subdirectory(tests)
endif()
