# SPDX-License-Identifier: GPL-3.0-or-later
add_library(synbench_test_ai STATIC ai_test.h ai_test.cpp)
target_link_libraries(synbench_test_ai PUBLIC synbench_core synbench_test_common)
