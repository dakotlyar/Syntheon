# SPDX-License-Identifier: GPL-3.0-or-later
add_library(synbench_test_npu STATIC npu_test.h npu_test.cpp)
target_link_libraries(synbench_test_npu PUBLIC synbench_core synbench_test_common)
