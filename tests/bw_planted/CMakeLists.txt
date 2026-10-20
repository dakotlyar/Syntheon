# SPDX-License-Identifier: GPL-3.0-or-later
# The planted store-rule violations (see planted_stores.cpp), built like the
# real bandwidth objects. An object library: never linked into anything.
add_library(synbench_bw_planted OBJECT planted_stores.cpp)
target_compile_options(synbench_bw_planted PRIVATE $<IF:$<BOOL:${MSVC}>,/O2,-O2>)
set_target_properties(synbench_bw_planted PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
