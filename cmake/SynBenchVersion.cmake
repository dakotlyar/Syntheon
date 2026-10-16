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
# Dev builds get "-dev+<sha>" appended to the display version. The sha is read
# at configure time AND again at every build (GenerateVersionHeader.cmake via the
# synbench_version_header target), so it never goes stale; release builds
# (SYNBENCH_RELEASE_BUILD=ON) don't carry it in the version string.
function(synbench_generate_version_header)
    find_package(Git QUIET)
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _os)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _cpu)
    if(_cpu MATCHES "^(x86_64|amd64|x64)$")
        set(_arch "x64")
    elseif(_cpu MATCHES "^(aarch64|arm64)$")
        set(_arch "arm64")
    else()
        set(_arch "${_cpu}")
    endif()
    if(SYNBENCH_RELEASE_BUILD)
        set(_release ON)
    else()
        set(_release OFF)
    endif()
    set(_header "${PROJECT_BINARY_DIR}/generated/synbench_version.h")
    set(_args
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DOUTPUT=${_header}
        -DTEMPLATE=${PROJECT_SOURCE_DIR}/cmake/synbench_version.h.in
        -DVERSION=${SYNBENCH_VERSION}
        -DFILE_VERSION=${SYNBENCH_FILE_VERSION}
        -DMSI_VERSION=${SYNBENCH_MSI_VERSION}
        -DRELEASE_BUILD=${_release}
        -DOS=${_os}
        -DARCH=${_arch}
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}")
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated")
    # Once now, so the header exists for the first build...
    execute_process(COMMAND "${CMAKE_COMMAND}" ${_args} -P "${PROJECT_SOURCE_DIR}/cmake/GenerateVersionHeader.cmake")
    # ...and at every build (cheap; rewrites the file only when the sha changed).
    add_custom_target(synbench_version_header ALL
        COMMAND "${CMAKE_COMMAND}" ${_args} -P "${PROJECT_SOURCE_DIR}/cmake/GenerateVersionHeader.cmake"
        BYPRODUCTS "${_header}"
        COMMENT "Version stamp"
        VERBATIM)

    add_library(synbench_version INTERFACE)
    target_include_directories(synbench_version INTERFACE "${PROJECT_BINARY_DIR}/generated")
    add_dependencies(synbench_version synbench_version_header)
endfunction()
