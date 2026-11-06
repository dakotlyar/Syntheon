# SPDX-License-Identifier: GPL-3.0-or-later
add_library(syntheon_test_ai STATIC ai_test.h ai_test.cpp)
target_link_libraries(syntheon_test_ai PUBLIC syntheon_core syntheon_test_common)
