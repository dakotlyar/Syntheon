# SPDX-License-Identifier: GPL-3.0-or-later
# The Storage test's per-OS async I/O engines (Phase 4b design §4.1, D7).
# The sync engine ships everywhere (ARM64 / Q1T1 / the fallback); on Linux x64
# the async engine is io_uring (SYNTHEON_HAVE_LIBURING) or libaio
# (SYNTHEON_HAVE_LIBAIO), detected in cmake/StorageIo.cmake (included before
# the notices are written); the overlapped engine on Windows is task 9.
target_sources(syntheon_test_storage PRIVATE
    engine.h engine_sync.cpp pickEngine.cpp
)

if(SYNTHEON_HAVE_LIBURING)
    target_sources(syntheon_test_storage PRIVATE engine_linux_uring.cpp)
    target_compile_definitions(syntheon_test_storage PUBLIC SYNTHEON_HAVE_LIBURING=1)
    target_link_libraries(syntheon_test_storage PRIVATE "${SYNTHEON_LIBURING_LIBRARY}")
elseif(SYNTHEON_HAVE_LIBAIO)
    target_sources(syntheon_test_storage PRIVATE engine_linux_aio.cpp)
    target_compile_definitions(syntheon_test_storage PUBLIC SYNTHEON_HAVE_LIBAIO=1)
    target_link_libraries(syntheon_test_storage PRIVATE "${SYNTHEON_LIBAIO_LIBRARY}")
endif()

if(WIN32)
    target_sources(syntheon_test_storage PRIVATE engine_win.cpp)
endif()
