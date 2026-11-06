# SPDX-License-Identifier: GPL-3.0-or-later
# The kernels: their own target, for their compile flags (Phase 3 design §1).
add_subdirectory(kernels)

add_library(syntheon_test_ram STATIC
    ram_test.h ram_test.cpp ram_detail.cpp
    # The score (§10; pure).
    formula_v1.h formula_v1.cpp
    # Buffers on huge / large pages (Phase 3 design §6, P7).
    memory/huge_buffer.h memory/huge_buffer.cpp
    # The latency sweep's grid, scored targets and knee fallback (§5.2; pure).
    sweep/grid.h sweep/grid.cpp
    sweep/targets.h sweep/targets.cpp
    sweep/knees.h sweep/knees.cpp
    sweep/warmup.h sweep/warmup.cpp
    # Loaded latency: the load generator and the five-level measurement (§5.4).
    loaded/load_generator.h loaded/load_generator.cpp
    loaded/loaded_latency.h loaded/loaded_latency.cpp
    # The memory configuration readout from the run's snapshot (§7; pure).
    config/readout.h config/readout.cpp
)
if(WIN32)
    target_sources(syntheon_test_ram PRIVATE memory/huge_buffer_win.cpp)
else()
    target_sources(syntheon_test_ram PRIVATE memory/huge_buffer_linux.cpp)
endif()
target_link_libraries(syntheon_test_ram PUBLIC syntheon_core syntheon_test_common syntheon_isa syntheon_ram_kernels
                                               syntheon_test_engine)
find_package(Threads REQUIRED)
target_link_libraries(syntheon_test_ram PRIVATE Threads::Threads)
