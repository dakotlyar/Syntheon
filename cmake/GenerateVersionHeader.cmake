# SPDX-License-Identifier: GPL-3.0-or-later
# Run with cmake -P at configure time AND at every build (the synbench_version_header
# target), so a dev build's "-dev+<sha>" follows HEAD without a reconfigure. The
# header is only rewritten when its content changes, so an unchanged HEAD
# recompiles nothing.
#
# Inputs (-D): SOURCE_DIR, OUTPUT, TEMPLATE, VERSION, FILE_VERSION, MSI_VERSION,
#              RELEASE_BUILD (ON/OFF), OS, ARCH, GIT_EXECUTABLE (may be empty)
# Run with cmake -P: the policies of the project's minimum (a script gets
# none from the project, and CMake 3.x would run it with OLD ones).
cmake_minimum_required(VERSION 3.25)
set(_sha "unknown")
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE _out OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc ERROR_QUIET)
    if(_rc EQUAL 0 AND _out)
        set(_sha "${_out}")
    endif()
endif()
set(SYNBENCH_VERSION "${VERSION}")
set(SYNBENCH_FILE_VERSION "${FILE_VERSION}")
set(SYNBENCH_MSI_VERSION "${MSI_VERSION}")
set(SYNBENCH_GIT_SHA "${_sha}")
if(RELEASE_BUILD)
    set(SYNBENCH_DISPLAY_VERSION "${VERSION}")
else()
    set(SYNBENCH_DISPLAY_VERSION "${VERSION}-dev+${_sha}")
endif()
set(SYNBENCH_BUILD_ID "${OS}-${ARCH} ${_sha}")
configure_file("${TEMPLATE}" "${OUTPUT}.tmp" @ONLY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUTPUT}.tmp" "${OUTPUT}")
file(REMOVE "${OUTPUT}.tmp")
