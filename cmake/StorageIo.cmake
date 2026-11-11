# SPDX-License-Identifier: GPL-3.0-or-later
# The Storage test's async I/O backends (Phase 4b design §4.1, D7): io_uring
# (preferred) and libaio (fallback) on Linux x64, detected once and shared with
# src/tests/storage/io/ (which compiles the engines) and tests/CMakeLists.txt
# (which checks the third-party notices). Included before the notices are
# written so a linked backend lands in THIRD-PARTY-NOTICES.txt.
#
# A Linux x64 *release* build (the packaging preset) must not silently ship
# sync-only: liburing is required there, as an explicit error, not a fallback.
# Development and CI builds keep the D7 fallback chain (uring -> aio -> sync).

set(SYNTHEON_HAVE_LIBURING OFF)
set(SYNTHEON_HAVE_LIBAIO OFF)
set(SYNTHEON_STORAGE_LINUX_X64 OFF)
set(SYNTHEON_LIBURING_LIBRARY "")
set(SYNTHEON_LIBAIO_LIBRARY "")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sb_io_cpu)
    if(_sb_io_cpu MATCHES "^(x86_64|amd64)$")
        set(SYNTHEON_STORAGE_LINUX_X64 ON)

        include(CheckIncludeFileCXX)
        check_include_file_cxx("liburing.h" _sb_have_uring_h)
        if(_sb_have_uring_h)
            find_library(_sb_uring_lib NAMES uring)
        endif()
        check_include_file_cxx("libaio.h" _sb_have_aio_h)
        if(_sb_have_aio_h)
            find_library(_sb_aio_lib NAMES aio)
        endif()

        if(_sb_uring_lib)
            set(SYNTHEON_HAVE_LIBURING ON)
            set(SYNTHEON_LIBURING_LIBRARY "${_sb_uring_lib}")
        elseif(_sb_aio_lib)
            set(SYNTHEON_HAVE_LIBAIO ON)
            set(SYNTHEON_LIBAIO_LIBRARY "${_sb_aio_lib}")
        endif()

        # A shipped Linux x64 release must have the real async engine.
        if(SYNTHEON_RELEASE_BUILD AND NOT CMAKE_CROSSCOMPILING AND NOT SYNTHEON_HAVE_LIBURING)
            message(FATAL_ERROR "liburing (liburing-dev) is required for the Linux x64 release build: the "
                                "Storage test's Q8/Q32 patterns must not silently fall back to sync I/O.")
        endif()

        # Notices for whichever backend is actually linked, so the AppImage
        # bundles the licence text alongside the bundled library.
        if(SYNTHEON_HAVE_LIBURING)
            syntheon_add_notice(NAME "liburing" LICENCE "MIT"
                FILE "${PROJECT_SOURCE_DIR}/packaging/licenses/liburing-MIT.txt")
        endif()
        if(SYNTHEON_HAVE_LIBAIO)
            syntheon_add_notice(NAME "libaio" LICENCE "LGPL-2.1-only"
                FILE "${PROJECT_SOURCE_DIR}/packaging/licenses/libaio-LGPL-2.1.txt")
        endif()
    endif()
endif()
