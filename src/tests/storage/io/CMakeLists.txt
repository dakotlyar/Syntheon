# SPDX-License-Identifier: GPL-3.0-or-later
# The Storage test's per-OS async I/O engines (Phase 4b design §4.1, D7).
# The sync engine ships everywhere (ARM64 / Q1T1 / the fallback); on Linux x64
# the async engine is io_uring when liburing is present, else libaio, else
# sync — detected here so a cross toolchain without the header or library
# simply builds the sync engine (N11). The overlapped engine on Windows is
# added by task 9.
target_sources(syntheon_test_storage PRIVATE
    engine.h engine_sync.cpp pickEngine.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sb_cpu)
    if(_sb_cpu MATCHES "^(x86_64|amd64)$")
        include(CheckIncludeFileCXX)
        check_include_file_cxx("liburing.h" _sb_have_uring_h)
        check_include_file_cxx("libaio.h" _sb_have_aio_h)
        if(_sb_have_uring_h)
            find_library(_sb_uring_lib NAMES uring)
        endif()
        if(_sb_have_aio_h)
            find_library(_sb_aio_lib NAMES aio)
        endif()
        if(_sb_uring_lib)
            # io_uring preferred (D7).
            target_sources(syntheon_test_storage PRIVATE engine_linux_uring.cpp)
            target_compile_definitions(syntheon_test_storage PUBLIC SYNTHEON_HAVE_LIBURING=1)
            target_link_libraries(syntheon_test_storage PRIVATE "${_sb_uring_lib}")
        elseif(_sb_aio_lib)
            # libaio fallback.
            target_sources(syntheon_test_storage PRIVATE engine_linux_aio.cpp)
            target_compile_definitions(syntheon_test_storage PUBLIC SYNTHEON_HAVE_LIBAIO=1)
            target_link_libraries(syntheon_test_storage PRIVATE "${_sb_aio_lib}")
        endif()
    endif()
endif()
