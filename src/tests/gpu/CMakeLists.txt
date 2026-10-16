# SPDX-License-Identifier: GPL-3.0-or-later
add_library(synbench_test_gpu STATIC gpu_test.h gpu_test.cpp)
target_link_libraries(synbench_test_gpu PUBLIC synbench_core synbench_test_common)
