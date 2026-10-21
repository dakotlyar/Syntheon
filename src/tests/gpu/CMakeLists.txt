# SPDX-License-Identifier: GPL-3.0-or-later
# The GPU test's pure model: ids, metric keys, frame statistics (Phase 4a design §1).
# No GUI and no GPU, so it builds and tests everywhere.
add_library(synbench_gpu_model STATIC
    model/ids.h model/ids.cpp
    model/frame_stats.h model/frame_stats.cpp
    model/best_of.h model/best_of.cpp
)
target_link_libraries(synbench_gpu_model PUBLIC synbench_core Qt6::Core)

add_library(synbench_test_gpu STATIC gpu_test.h gpu_test.cpp)
target_link_libraries(synbench_test_gpu PUBLIC synbench_core synbench_test_common synbench_gpu_model)
