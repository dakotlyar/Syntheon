# SPDX-License-Identifier: GPL-3.0-or-later
add_library(synbench_test_cpu STATIC cpu_test.h cpu_test.cpp)
target_link_libraries(synbench_test_cpu PUBLIC synbench_core PRIVATE synbench_test_common)
