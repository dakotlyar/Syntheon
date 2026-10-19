# SPDX-License-Identifier: GPL-3.0-or-later
add_library(synbench_test_ram STATIC
    ram_test.h ram_test.cpp
    # Buffers on huge / large pages (Phase 3 design §6, P7).
    memory/huge_buffer.h memory/huge_buffer.cpp
)
if(WIN32)
    target_sources(synbench_test_ram PRIVATE memory/huge_buffer_win.cpp)
else()
    target_sources(synbench_test_ram PRIVATE memory/huge_buffer_linux.cpp)
endif()
target_link_libraries(synbench_test_ram PUBLIC synbench_core synbench_test_common)
