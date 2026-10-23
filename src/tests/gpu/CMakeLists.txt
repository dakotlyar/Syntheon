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
    model/windows_gpus.h model/windows_gpus.cpp
    model/gl_env.h model/gl_env.cpp
    model/hags.h model/hags.cpp
)
target_link_libraries(synbench_gpu_model PUBLIC synbench_core Qt6::Core)
if(WIN32)
    target_link_libraries(synbench_gpu_model PRIVATE advapi32)  # RegGetValueW: gpu.hags (hags.cpp)
endif()

add_library(synbench_test_gpu STATIC gpu_test.h gpu_test.cpp gpu_detail.cpp formula_v1.h formula_v1.cpp client/gpu_client.h client/gpu_client.cpp)
target_link_libraries(synbench_test_gpu PUBLIC synbench_core synbench_test_common synbench_gpu_model synbench_test_engine synbench_sysinfo Qt6::Core Qt6::Gui)

# The worker's shaders: the Windows build's DXIL and DXBC (§13.1).
add_subdirectory(worker/shaders)

# §10's `shaders` metric: the committed SPIR-V/GLSL stamp (first 4 hex digits) and, on Windows, the SDK compilers'
# versions (found by worker/shaders/CMakeLists.txt).
set(_stamp_file "${CMAKE_CURRENT_SOURCE_DIR}/worker/shaders/generated/STAMP")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_stamp_file}")
file(STRINGS "${_stamp_file}" _stamp LIMIT_COUNT 1)
string(SUBSTRING "${_stamp}" 0 4 _stamp4)
target_compile_definitions(synbench_test_gpu PRIVATE SYNBENCH_GPU_SHADER_STAMP="${_stamp4}")
if(WIN32)
    target_compile_definitions(synbench_test_gpu PRIVATE
        SYNBENCH_GPU_DXC_VERSION="${SYNBENCH_GPU_DXC_VERSION}" SYNBENCH_GPU_FXC_VERSION="${SYNBENCH_GPU_FXC_VERSION}")
endif()

# The scene content: pure C++, no Qt.
add_subdirectory(worker/scene)

# The worker executable and its backends (after the shaders and the scene).
add_subdirectory(worker)
