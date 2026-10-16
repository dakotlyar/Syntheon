# SPDX-License-Identifier: GPL-3.0-or-later
add_library(synbench_test_storage STATIC storage_test.h storage_test.cpp)
target_link_libraries(synbench_test_storage PUBLIC synbench_core synbench_test_common)
