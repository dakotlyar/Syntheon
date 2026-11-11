# SPDX-License-Identifier: GPL-3.0-or-later
# The Storage test's per-OS async I/O engines (Phase 4b design §4.1, D7).
# The sync engine ships everywhere (ARM64 / Q1T1 / the fallback); the io_uring
# engine on Linux x64 when liburing is present, the libaio fallback (task 7),
# and the overlapped engine on Windows (task 9).
target_sources(syntheon_test_storage PRIVATE
    engine.h engine_sync.cpp
)

# io_uring (Linux x64 only, D7): detected so a cross toolchain without the
# header or library simply builds the sync engine (N11).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sb_cpu)
    if(_sb_cpu MATCHES "^(x86_64|amd64)$")
        include(CheckIncludeFileCXX)
        check_include_file_cxx("liburing.h" _sb_have_uring_h)
        if(_sb_have_uring_h)
            find_library(_sb_uring_lib NAMES uring)
            if(_sb_uring_lib)
                target_sources(syntheon_test_storage PRIVATE engine_linux_uring.cpp)
                target_compile_definitions(syntheon_test_storage PUBLIC SYNTHEON_HAVE_LIBURING=1)
                target_link_libraries(syntheon_test_storage PRIVATE "${_sb_uring_lib}")
            endif()
        endif()
    endif()
endif()
