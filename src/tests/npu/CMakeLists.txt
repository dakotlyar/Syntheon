# SPDX-License-Identifier: GPL-3.0-or-later
add_library(syntheon_test_npu STATIC npu_test.h npu_test.cpp)
target_link_libraries(syntheon_test_npu PUBLIC syntheon_core syntheon_test_common)
