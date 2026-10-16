# SPDX-License-Identifier: GPL-3.0-or-later
add_library(synbench_test_ram STATIC ram_test.h ram_test.cpp)
target_link_libraries(synbench_test_ram PUBLIC synbench_core synbench_test_common)
