# SPDX-License-Identifier: GPL-3.0-or-later
# The GPU test's pure model: ids, metric keys, frame statistics (Phase 4a design §1).
# No GUI and no GPU, so it builds and tests everywhere.
add_library(synbench_gpu_model STATIC
    model/ids.h model/ids.cpp
    model/frame_stats.h model/frame_stats.cpp
    model/best_of.h model/best_of.cpp
    model/render_size.h model/render_size.cpp
    model/gpu_identity.h model/gpu_identity.cpp
    model/protocol.h model/protocol.cpp
    model/linux_gpus.h model/linux_gpus.cpp
)
target_link_libraries(synbench_gpu_model PUBLIC synbench_core Qt6::Core)

add_library(synbench_test_gpu STATIC gpu_test.h gpu_test.cpp client/gpu_client.h client/gpu_client.cpp)
target_link_libraries(synbench_test_gpu PUBLIC synbench_core synbench_test_common synbench_gpu_model Qt6::Core)

# The worker's shaders: the Windows build's DXIL and DXBC (§13.1).
add_subdirectory(worker/shaders)

# The scene content: pure C++, no Qt.
add_subdirectory(worker/scene)

# The worker executable and its backends (after the shaders and the scene).
add_subdirectory(worker)
