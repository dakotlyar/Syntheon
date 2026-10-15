# SPDX-License-Identifier: GPL-3.0-or-later
add_library(synbench_test_common STATIC skeleton_test.h skeleton_test.cpp)
target_link_libraries(synbench_test_common PUBLIC synbench_core)
