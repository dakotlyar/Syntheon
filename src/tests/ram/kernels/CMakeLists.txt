# SPDX-License-Identifier: GPL-3.0-or-later
# The RAM test's kernels (Phase 3 design §1, §5, §9): no Qt, linked into the
# RAM test. -O2 / /O2 in every build type and no LTO/IPO, so a Debug build
# measures the same code (P18); no FP rules: the kernels are integer-only.
# The ram_kernel_flags CTest checks the flags that reach each TU in
# compile_commands.json.
if(MSVC)
    # Debug's /Od and /Ob0 would undo /O2, and /RTC1 can't be combined with
    # it. Directory scope: only this folder.
    foreach(_lang C CXX)
        foreach(_flag /RTC1 /RTCsu /RTCs /RTCu /RTCc /Od /Ob0)
            string(REPLACE "${_flag}" "" CMAKE_${_lang}_FLAGS_DEBUG "${CMAKE_${_lang}_FLAGS_DEBUG}")
        endforeach()
    endforeach()
    set(SYNBENCH_RAM_KERNEL_FLAGS /O2)
else()
    set(SYNBENCH_RAM_KERNEL_FLAGS -O2)
endif()

add_library(synbench_ram_kernels STATIC
    # The pointer chase (§5.1, P3).
    cycle_builder.h cycle_builder.cpp
    chase.h chase.cpp chase_loop.cpp
)
target_link_libraries(synbench_ram_kernels PUBLIC synbench_isa)
target_compile_options(synbench_ram_kernels PRIVATE ${SYNBENCH_RAM_KERNEL_FLAGS})
# No Qt here: no moc pass (qt_standard_project_setup turns it on everywhere).
set_target_properties(synbench_ram_kernels PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
