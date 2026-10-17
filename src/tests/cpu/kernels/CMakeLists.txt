# SPDX-License-Identifier: GPL-3.0-or-later
# The CPU test's kernels (Phase 2 design §5, §6.2, P7): no Qt, linked into
# the CPU test and the golden-table generator.
#
# P7, for every TU here and in every build type: -O2 / /O2, no
# auto-vectorisation, no FP contraction, no LTO/IPO, so the integer, FP,
# primes, sorting and physics groups measure the scalar core the same way with
# GCC, Clang and MSVC, and the FP results are bit-exact. MSVC has no switch
# for vectorisation or contraction: the TUs add `#pragma fp_contract(off)`
# and `#pragma loop(no_vector)` (kernel_common.h). The cpu_kernel_flags CTest
# checks the flags that reach each TU in compile_commands.json.
if(MSVC)
    # Debug's /Od and /Ob0 would undo /O2, and /RTC1 can't be combined with
    # it (§4.4: stripped for the kernels). Directory scope: only this folder.
    foreach(_lang C CXX)
        foreach(_flag /RTC1 /RTCsu /RTCs /RTCu /RTCc /Od /Ob0)
            string(REPLACE "${_flag}" "" CMAKE_${_lang}_FLAGS_DEBUG "${CMAKE_${_lang}_FLAGS_DEBUG}")
        endforeach()
    endforeach()
    set(SYNBENCH_SCALAR_KERNEL_FLAGS /O2 /fp:precise)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(SYNBENCH_SCALAR_KERNEL_FLAGS -O2 -fno-vectorize -fno-slp-vectorize -ffp-contract=off)
else()
    set(SYNBENCH_SCALAR_KERNEL_FLAGS -O2 -fno-tree-vectorize -ffp-contract=off)
endif()

add_library(synbench_cpu_kernels STATIC
    kernel_common.h
    prng.h
    int_kernel.h int_kernel.cpp
    fp_kernel.h fp_kernel.cpp
    primes_kernel.h primes_kernel.cpp
)
target_link_libraries(synbench_cpu_kernels PUBLIC synbench_isa)
target_compile_options(synbench_cpu_kernels PRIVATE ${SYNBENCH_SCALAR_KERNEL_FLAGS})
set_target_properties(synbench_cpu_kernels PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
