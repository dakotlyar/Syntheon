# SPDX-License-Identifier: GPL-3.0-or-later
# The planted ODR violation (see planted_wide.cpp), with the first wide ISA's
# flags. An object library: never linked into anything.
add_library(syntheon_simd_planted OBJECT planted_wide.cpp)
target_compile_options(syntheon_simd_planted PRIVATE ${SYNTHEON_PLANTED_FLAGS})
set_target_properties(syntheon_simd_planted PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# ...and one whose only fault is a dynamic initialiser (AP-5 L1).
add_library(syntheon_simd_planted_init OBJECT planted_init.cpp)
target_compile_options(syntheon_simd_planted_init PRIVATE ${SYNTHEON_PLANTED_FLAGS})
set_target_properties(syntheon_simd_planted_init PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
