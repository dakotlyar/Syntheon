# SPDX-License-Identifier: GPL-3.0-or-later
# Third-party C libraries (Phase 2 design §11, D4, P16): pinned release
# tarballs, each checked against its SHA-256, built from source by CMake as
# static libraries, so every platform runs identical code. Their own
# programs, tests, shared libraries and install rules are off; they build with
# SynBench's -march baseline but with warnings off, and their headers are
# SYSTEM headers.
#
# Pins (changing one changes measured results: bump the CPU testVersion):
#   lib    version  verified 2026-10-16 against
#   zlib   1.3.2    Mark Adler's signature (.asc, key 5ED4 6A67 21D3 6558 7791
#                   E2AA 783F CD8E 58BC AFBA) and the SHA-256 on zlib.net
#   zstd   1.5.7    the Zstandard release signature (.sig, key 4EF4 AC63 455F
#                   C9F4 545D 9B7D EF8F E995 28B5 2FFD) and the release's .sha256
#   lz4    1.10.0   the release's .sha256 (LZ4 publishes no signature), equal to
#                   GitHub's archive of tag v1.10.0
#   xz     5.8.4    Lasse Collin's signature (.sig, key 3690 C240 CE51 B467 0D30
#                   AD1C 38EE 757D 6918 4620, as published on tukaani.org).
#                   NEVER 5.6.0 or 5.6.1 (CVE-2024-3094, the backdoored releases).
#
# The sources land in the build tree (_deps/); set FETCHCONTENT_BASE_DIR to
# keep them elsewhere. Targets for the rest of the build:
#   synbench::zlib  synbench::zstd  synbench::lz4  synbench::lzma
include_guard(GLOBAL)
include(FetchContent)
include(ThirdPartyNotices)

set(SYNBENCH_ZLIB_VERSION 1.3.2)
set(SYNBENCH_ZSTD_VERSION 1.5.7)
set(SYNBENCH_LZ4_VERSION 1.10.0)
set(SYNBENCH_XZ_VERSION 5.8.4)
if(SYNBENCH_XZ_VERSION MATCHES "^5\\.6\\.[01]$")
    message(FATAL_ERROR "xz ${SYNBENCH_XZ_VERSION} is the backdoored release (CVE-2024-3094)")
endif()

FetchContent_Declare(synbench_zlib
    URL "https://github.com/madler/zlib/releases/download/v${SYNBENCH_ZLIB_VERSION}/zlib-${SYNBENCH_ZLIB_VERSION}.tar.gz"
    URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
FetchContent_Declare(synbench_zstd
    URL "https://github.com/facebook/zstd/releases/download/v${SYNBENCH_ZSTD_VERSION}/zstd-${SYNBENCH_ZSTD_VERSION}.tar.gz"
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR build/cmake
    SYSTEM)
FetchContent_Declare(synbench_lz4
    URL "https://github.com/lz4/lz4/releases/download/v${SYNBENCH_LZ4_VERSION}/lz4-${SYNBENCH_LZ4_VERSION}.tar.gz"
    URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR build/cmake
    SYSTEM)
FetchContent_Declare(synbench_xz
    URL "https://github.com/tukaani-project/xz/releases/download/v${SYNBENCH_XZ_VERSION}/xz-${SYNBENCH_XZ_VERSION}.tar.gz"
    URL_HASH SHA256=0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)

# A function, so the option variables below stay out of the rest of the build.
function(synbench_add_third_party)
    # Each library's own switches: the static library only.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(ZLIB_BUILD_TESTING OFF)
    set(ZLIB_BUILD_SHARED OFF)
    set(ZLIB_BUILD_STATIC ON)
    set(ZLIB_INSTALL OFF)
    set(ZSTD_BUILD_STATIC ON)
    set(ZSTD_BUILD_SHARED OFF)
    set(ZSTD_BUILD_PROGRAMS OFF)
    set(ZSTD_BUILD_TESTS OFF)
    set(ZSTD_BUILD_CONTRIB OFF)
    set(ZSTD_LEGACY_SUPPORT OFF)       # no pre-v0.8 frame formats
    set(ZSTD_MULTITHREAD_SUPPORT OFF)  # SynBench does its own threading (§5.4)
    set(LZ4_BUILD_CLI OFF)             # the CLI is GPL-2.0; the library is BSD-2-Clause
    set(LZ4_BUILD_LEGACY_LZ4C OFF)
    set(XZ_THREADS no)                 # our own threading again
    set(XZ_NLS OFF)
    set(XZ_DOC OFF)
    set(XZ_DOXYGEN OFF)
    foreach(tool XZ_TOOL_XZ XZ_TOOL_XZDEC XZ_TOOL_LZMADEC XZ_TOOL_LZMAINFO XZ_TOOL_SCRIPTS XZ_TOOL_SYMLINKS XZ_TOOL_SYMLINKS_LZMA)
        set(${tool} OFF)
    endforeach()
    # Policy: let these normal variables win over the libraries' option() calls.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    # lz4 1.10.0 still asks for CMake 3.5, which newer CMake deprecates; run it
    # with 3.10's policies (CMake 4.0+ reads this; older ones just warn).
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

    FetchContent_MakeAvailable(synbench_zlib synbench_zstd synbench_lz4 synbench_xz)

    # Only what SynBench links gets built, and none of it is installed.
    foreach(dep synbench_zlib synbench_zstd synbench_lz4 synbench_xz)
        FetchContent_GetProperties(${dep} SOURCE_DIR _src BINARY_DIR _bin)
        set_property(DIRECTORY "${_bin}" PROPERTY EXCLUDE_FROM_ALL ON)
        set(${dep}_SOURCE_DIR "${_src}" PARENT_SCOPE)
    endforeach()
endfunction()
synbench_add_third_party()

# Warnings off for the library targets: their options start as this
# directory's (our -W/ /W4 set plus the -march baseline from
# CompilerFlags.cmake) and the libraries add their own (xz: /W4 on MSVC).
# The warning switches are taken out and "no warnings" put last; the
# baseline and everything else stay.
foreach(_tgt zlibstatic libzstd_static lz4_static liblzma)
    get_target_property(_all ${_tgt} COMPILE_OPTIONS)
    set(_opts "")
    foreach(_opt IN LISTS _all)
        # -Wa, / -Wl, / -Wp, pass options to the assembler, linker and
        # preprocessor (zstd: -Wa,--noexecstack); they stay.
        if(_opt MATCHES "^-W[alp]," OR NOT _opt MATCHES "^(-W|-pedantic|/W[0-4]$|/wd|/permissive-|/Zc:|/EHsc)")
            list(APPEND _opts "${_opt}")
        endif()
    endforeach()
    if(MSVC)
        list(APPEND _opts /w)
    else()
        list(APPEND _opts -w)
    endif()
    set_target_properties(${_tgt} PROPERTIES COMPILE_OPTIONS "${_opts}")
endforeach()

add_library(synbench::zlib ALIAS zlibstatic)
add_library(synbench::zstd ALIAS libzstd_static)
add_library(synbench::lz4 ALIAS lz4_static)
add_library(synbench::lzma ALIAS liblzma)

# Their licences, for THIRD-PARTY-NOTICES.txt.
synbench_add_notice(NAME "zlib" VERSION ${SYNBENCH_ZLIB_VERSION} LICENCE "Zlib"
    FILE "${synbench_zlib_SOURCE_DIR}/LICENSE")
synbench_add_notice(NAME "Zstandard (zstd)" VERSION ${SYNBENCH_ZSTD_VERSION} LICENCE "BSD-3-Clause"
    FILE "${synbench_zstd_SOURCE_DIR}/LICENSE"
    NOTE "zstd is dual-licensed (BSD-3-Clause or GPL-2.0-only). SynBench uses it under BSD-3-Clause.")
synbench_add_notice(NAME "LZ4 (library)" VERSION ${SYNBENCH_LZ4_VERSION} LICENCE "BSD-2-Clause"
    FILE "${synbench_lz4_SOURCE_DIR}/lib/LICENSE"
    NOTE "Only the LZ4 library (lib/) is used. The LZ4 command-line program, which is GPL-2.0, is not built.")
synbench_add_notice(NAME "XZ Utils (liblzma)" VERSION ${SYNBENCH_XZ_VERSION} LICENCE "0BSD"
    FILE "${synbench_xz_SOURCE_DIR}/COPYING.0BSD"
    NOTE "Only liblzma is used. The xz tools and scripts are not built.")
