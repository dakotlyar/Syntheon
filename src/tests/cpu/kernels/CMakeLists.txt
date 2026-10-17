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
    simd_kernel.h simd_kernel.cpp
    simd/simd_common.h
    sort_kernel.h sort_kernel.cpp
    nbody_kernel.h nbody_kernel.cpp
    block_hash.h block_hash.cpp
)
target_link_libraries(synbench_cpu_kernels PUBLIC synbench_isa)

# The SIMD paths (§5.3, P8, P9): one OBJECT library per ISA, each with its own
# arch flags on top of P7's, built for its architecture only. The wide ones
# (wider than the build's baseline) follow the ODR rule in simd/simd_common.h;
# the simd_odr CTest checks their objects. tests/CMakeLists.txt finds them
# through the global property SYNBENCH_SIMD_ISAS and each target's
# SYNBENCH_ARCH_FLAGS and SYNBENCH_WIDE.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sb_cpu)
set(SYNBENCH_SIMD_ISAS "")
if(_sb_cpu MATCHES "^(x86_64|amd64|x64)$")
    set(SYNBENCH_SIMD_ISAS sse42 avx2 avx512)
    set(SYNBENCH_SIMD_sse42_FLAGS "")  # the x86-64-v2 baseline
    if(MSVC)
        set(SYNBENCH_SIMD_avx2_FLAGS /arch:AVX2)
        set(SYNBENCH_SIMD_avx512_FLAGS /arch:AVX512)
    else()
        set(SYNBENCH_SIMD_avx2_FLAGS -mavx2 -mfma)
        set(SYNBENCH_SIMD_avx512_FLAGS -mavx512f -mavx512bw -mavx512dq -mavx512vl -mfma)
    endif()
    set(SYNBENCH_SIMD_WIDE avx2 avx512)
elseif(_sb_cpu MATCHES "^(aarch64|arm64)$")
    set(SYNBENCH_SIMD_ISAS neon)
    set(SYNBENCH_SIMD_neon_FLAGS "")  # the armv8-a baseline
    set(SYNBENCH_SIMD_WIDE "")
    # SVE: Linux only (MSVC has no SVE intrinsics; isa::kSveInBuild agrees).
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND SYNBENCH_SIMD_ISAS sve)
        set(SYNBENCH_SIMD_sve_FLAGS -march=armv8.2-a+sve)
        list(APPEND SYNBENCH_SIMD_WIDE sve)
        target_compile_definitions(synbench_cpu_kernels PRIVATE SB_SIMD_SVE=1)
    endif()
endif()
foreach(_isa IN LISTS SYNBENCH_SIMD_ISAS)
    add_library(synbench_simd_${_isa} OBJECT simd/simd_${_isa}.cpp simd/simd_common.h)
    target_include_directories(synbench_simd_${_isa} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(synbench_simd_${_isa} PRIVATE ${SYNBENCH_SCALAR_KERNEL_FLAGS} ${SYNBENCH_SIMD_${_isa}_FLAGS})
    set_target_properties(synbench_simd_${_isa} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_sources(synbench_cpu_kernels PRIVATE $<TARGET_OBJECTS:synbench_simd_${_isa}>)
    set(_wide OFF)
    if(_isa IN_LIST SYNBENCH_SIMD_WIDE)
        set(_wide ON)
    endif()
    set_target_properties(synbench_simd_${_isa} PROPERTIES
        SYNBENCH_ARCH_FLAGS "${SYNBENCH_SIMD_${_isa}_FLAGS}" SYNBENCH_WIDE ${_wide})
    set_property(GLOBAL APPEND PROPERTY SYNBENCH_SIMD_ISAS ${_isa})
endforeach()
target_compile_options(synbench_cpu_kernels PRIVATE ${SYNBENCH_SCALAR_KERNEL_FLAGS})
# No Qt here: no moc pass (qt_standard_project_setup turns it on everywhere).
set_target_properties(synbench_cpu_kernels PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
