cmake_minimum_required(VERSION 3.25)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SynBenchVersion)
synbench_read_version("${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(SynBench
    VERSION ${SYNBENCH_VERSION}
    DESCRIPTION "Synthetic system benchmark"
    LANGUAGES CXX)

option(SYNBENCH_BUILD_TESTS "Build the unit tests" ON)
option(SYNBENCH_RELEASE_BUILD "Release build: no -dev+<sha> suffix on the version" OFF)
set(SYNBENCH_QT_VERSION "6.12.0" CACHE STRING "Qt version the build is pinned to (set by CMakePresets.json)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Executables go to <build>/bin so the QML module folder "SynBench" next to the
# app's sources can't collide with the "SynBench" executable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(CompilerFlags)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
if(NOT Qt6_VERSION VERSION_EQUAL SYNBENCH_QT_VERSION)
    message(WARNING "Found Qt ${Qt6_VERSION}, but the build is pinned to Qt ${SYNBENCH_QT_VERSION}")
endif()
qt_standard_project_setup(REQUIRES 6.12)

synbench_generate_version_header()

add_subdirectory(src)

if(SYNBENCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
