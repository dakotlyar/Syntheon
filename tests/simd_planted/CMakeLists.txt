# SPDX-License-Identifier: GPL-3.0-or-later
# The planted ODR violation (see planted_wide.cpp), with the first wide ISA's
# flags. An object library: never linked into anything.
add_library(synbench_simd_planted OBJECT planted_wide.cpp)
target_compile_options(synbench_simd_planted PRIVATE ${SYNBENCH_PLANTED_FLAGS})
set_target_properties(synbench_simd_planted PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
