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
    # Bandwidth (§5.3, P8, D4): the baseline glue; the paths are below.
    bw_kernels.h bw_kernels.cpp
    bw/bw_common.h
    # The loaded-latency load's delay (§5.4).
    spin.h spin.cpp
)
target_link_libraries(synbench_ram_kernels PUBLIC synbench_isa)
target_compile_options(synbench_ram_kernels PRIVATE ${SYNBENCH_RAM_KERNEL_FLAGS})
# No Qt here: no moc pass (qt_standard_project_setup turns it on everywhere).
set_target_properties(synbench_ram_kernels PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# The bandwidth paths (§5.3, P8): one OBJECT library per ISA, built for its
# architecture only, with its arch flags on top of the ones above. Every one
# follows the ODR rule in bw/bw_common.h (the simd_odr CTest checks them as
# "bw:" objects), and the x64 ones' store variants are checked in their code
# (the bw_stores CTest). tests/CMakeLists.txt finds them through the global
# property SYNBENCH_BW_ISAS and each target's SYNBENCH_ARCH_FLAGS and
# SYNBENCH_WIDE. GCC's loop-distribution and Clang's loop-idiom passes could
# turn a copy loop into a memcpy call (whose large-size path uses
# non-temporal stores): both are off here, and bw_stores rejects any call.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sb_cpu)
set(SYNBENCH_BW_ISAS "")
if(_sb_cpu MATCHES "^(x86_64|amd64|x64)$")
    set(SYNBENCH_BW_ISAS sse2 avx2)
    set(SYNBENCH_BW_sse2_FLAGS "")  # part of the x86-64-v2 baseline
    if(MSVC)
        set(SYNBENCH_BW_avx2_FLAGS /arch:AVX2)
    else()
        set(SYNBENCH_BW_avx2_FLAGS -mavx2)
    endif()
    set(SYNBENCH_BW_WIDE avx2)
elseif(_sb_cpu MATCHES "^(aarch64|arm64)$")
    set(SYNBENCH_BW_ISAS neon)
    set(SYNBENCH_BW_neon_FLAGS "")  # the armv8-a baseline
    set(SYNBENCH_BW_WIDE "")
endif()
if(MSVC)
    set(_bw_no_idiom "")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_bw_no_idiom -fno-builtin)
else()
    set(_bw_no_idiom -fno-tree-loop-distribute-patterns)
endif()
foreach(_isa IN LISTS SYNBENCH_BW_ISAS)
    add_library(synbench_bw_${_isa} OBJECT bw/bw_${_isa}.cpp bw/bw_common.h)
    target_include_directories(synbench_bw_${_isa} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(synbench_bw_${_isa} PRIVATE ${SYNBENCH_RAM_KERNEL_FLAGS} ${_bw_no_idiom}
                                                       ${SYNBENCH_BW_${_isa}_FLAGS})
    set_target_properties(synbench_bw_${_isa} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_sources(synbench_ram_kernels PRIVATE $<TARGET_OBJECTS:synbench_bw_${_isa}>)
    set(_wide OFF)
    if(_isa IN_LIST SYNBENCH_BW_WIDE)
        set(_wide ON)
    endif()
    set_target_properties(synbench_bw_${_isa} PROPERTIES
        SYNBENCH_ARCH_FLAGS "${SYNBENCH_BW_${_isa}_FLAGS}" SYNBENCH_WIDE ${_wide})
    set_property(GLOBAL APPEND PROPERTY SYNBENCH_BW_ISAS ${_isa})
endforeach()
