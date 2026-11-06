# SPDX-License-Identifier: GPL-3.0-or-later
add_library(syntheon_test_battery STATIC battery_test.h battery_test.cpp)
target_link_libraries(syntheon_test_battery PUBLIC syntheon_core syntheon_test_common)
