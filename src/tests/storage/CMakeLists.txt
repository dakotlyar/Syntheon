# SPDX-License-Identifier: GPL-3.0-or-later
add_library(syntheon_test_storage STATIC storage_test.h storage_test.cpp)
target_link_libraries(syntheon_test_storage PUBLIC syntheon_core syntheon_test_common)
