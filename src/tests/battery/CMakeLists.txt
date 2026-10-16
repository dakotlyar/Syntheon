# SPDX-License-Identifier: GPL-3.0-or-later
add_library(synbench_test_battery STATIC battery_test.h battery_test.cpp)
target_link_libraries(synbench_test_battery PUBLIC synbench_core synbench_test_common)
