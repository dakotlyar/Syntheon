# SPDX-License-Identifier: GPL-3.0-or-later
# Version handling (P22). The VERSION file (YYYY.MM.N) is the single source of truth.
#
# synbench_read_version(<file>) sets, in the caller's scope:
#   SYNBENCH_VERSION          2026.11.0
#   SYNBENCH_VERSION_MAJOR/MINOR/PATCH
#   SYNBENCH_FILE_VERSION     2026.11.0.0   (Windows file version)
#   SYNBENCH_MSI_VERSION      26.11.0       (MSI ProductVersion, D12)
function(synbench_read_version file)
    file(STRINGS "${file}" _v LIMIT_COUNT 1)
    string(STRIP "${_v}" _v)
    if(NOT _v MATCHES "^([0-9][0-9][0-9][0-9])\\.([0-9]+)\\.([0-9]+)$")
        message(FATAL_ERROR "VERSION must be YYYY.MM.N, got '${_v}'")
    endif()
    set(_major ${CMAKE_MATCH_1})
    set(_minor ${CMAKE_MATCH_2})
    set(_patch ${CMAKE_MATCH_3})
    math(EXPR _yy "${_major} % 100")
    set(SYNBENCH_VERSION "${_v}" PARENT_SCOPE)
    set(SYNBENCH_VERSION_MAJOR ${_major} PARENT_SCOPE)
    set(SYNBENCH_VERSION_MINOR ${_minor} PARENT_SCOPE)
    set(SYNBENCH_VERSION_PATCH ${_patch} PARENT_SCOPE)
    set(SYNBENCH_FILE_VERSION "${_v}.0" PARENT_SCOPE)
    set(SYNBENCH_MSI_VERSION "${_yy}.${_minor}.${_patch}" PARENT_SCOPE)
endfunction()

# synbench_generate_version_header() writes <build>/generated/synbench_version.h
# and creates the INTERFACE target synbench_version that exposes it.
#
# Dev builds get "-dev+<sha>" appended to the display version. The sha is read at
# configure time, so it goes stale until the next reconfigure; release builds
# (SYNBENCH_RELEASE_BUILD=ON) don't carry it in the version string.
function(synbench_generate_version_header)
    set(_sha "unknown")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            OUTPUT_VARIABLE _out OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _rc ERROR_QUIET)
        if(_rc EQUAL 0 AND _out)
            set(_sha "${_out}")
        endif()
    endif()

    if(SYNBENCH_RELEASE_BUILD)
        set(_display "${SYNBENCH_VERSION}")
    else()
        set(_display "${SYNBENCH_VERSION}-dev+${_sha}")
    endif()

    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _os)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _cpu)
    if(_cpu MATCHES "^(x86_64|amd64|x64)$")
        set(_arch "x64")
    elseif(_cpu MATCHES "^(aarch64|arm64)$")
        set(_arch "arm64")
    else()
        set(_arch "${_cpu}")
    endif()

    set(SYNBENCH_DISPLAY_VERSION "${_display}")
    set(SYNBENCH_GIT_SHA "${_sha}")
    set(SYNBENCH_BUILD_ID "${_os}-${_arch} ${_sha}")
    configure_file("${PROJECT_SOURCE_DIR}/cmake/synbench_version.h.in"
                   "${PROJECT_BINARY_DIR}/generated/synbench_version.h" @ONLY)

    add_library(synbench_version INTERFACE)
    target_include_directories(synbench_version INTERFACE "${PROJECT_BINARY_DIR}/generated")
endfunction()
