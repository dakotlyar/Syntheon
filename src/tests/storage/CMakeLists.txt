# SPDX-License-Identifier: GPL-3.0-or-later
add_library(syntheon_test_storage STATIC
    storage_test.h storage_test.cpp
    # The stable ids and metric keys (§10; pure).
    ids.h ids.cpp
    # The free-space guard, blocklist and wear arithmetic (§6; pure).
    req.h req.cpp
)
target_link_libraries(syntheon_test_storage PUBLIC syntheon_core syntheon_test_common)