# SPDX-License-Identifier: GPL-3.0-or-later
# The NPU test (Phase 4c design): a real NpuTest replacing the Phase 1 stub.
# The per-OS detection and the per-vendor engines live in focused subfolders
# (added as each batch lands); the engines load the user-installed vendor
# runtimes at runtime (dlopen/LoadLibrary), never at link time (P-VENDOR).
add_library(syntheon_test_npu STATIC
    npu_test.h npu_test.cpp
    # The stable ids and metric keys (§9; pure).
    ids.h ids.cpp
    # The score (§10; pure).
    formula_v1.h formula_v1.cpp
)
# The downloader (task 6) uses QtNetwork; Qt6::Network is found up top (§1.2).
target_link_libraries(syntheon_test_npu PUBLIC syntheon_core syntheon_test_common PRIVATE Qt6::Network)

add_subdirectory(detect)
add_subdirectory(models)
