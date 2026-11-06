# SPDX-License-Identifier: GPL-3.0-or-later
add_library(syntheon_test_common STATIC
    stub_clock.h stub_clock.cpp
    stub_test.h stub_test.cpp
)
target_link_libraries(syntheon_test_common PUBLIC syntheon_core)
