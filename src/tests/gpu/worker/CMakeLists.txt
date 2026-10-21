# SPDX-License-Identifier: GPL-3.0-or-later
# synbench-gpu, the GPU test's worker (Phase 4a design §1, §2): one process per
# API, so a driver crash or a GPU hang never takes SynBench down.
#
# synbench_gpu_backends: Qt-free (P2): the backend interface, the runner, the
# signature and the backends themselves (null; Vulkan from task 10; D3D12,
# D3D11 and OpenGL from tasks 18, 20 and 21).
add_library(synbench_gpu_backends STATIC
    backend.h
    runner.h runner.cpp
    signature.h signature.cpp
    null/null_backend.h null/null_backend.cpp
    vulkan/vk_context.h vulkan/vk_context.cpp
    vulkan/vk_probe.h vulkan/vk_probe.cpp
    vulkan/vk_resources.h vulkan/vk_resources.cpp
    vulkan/vk_swapchain.h vulkan/vk_swapchain.cpp
    vulkan/vk_backend.h vulkan/vk_backend.cpp
    vulkan/vk_workload_state.h vulkan/vk_workloads.cpp
)
if(WIN32)
    # System32's vulkan-1.dll for volk (P-WIN: Windows-only code in its own file).
    target_sources(synbench_gpu_backends PRIVATE vulkan/vk_loader.h vulkan/vk_loader_win.cpp)
endif()
target_link_libraries(synbench_gpu_backends PUBLIC synbench_gpu_scene synbench::volk)
set_target_properties(synbench_gpu_backends PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# The Windows build's DXIL/DXBC includes (worker/shaders/CMakeLists.txt): the
# target, not the generated files, so they are never listed as sources here.
if(TARGET synbench_gpu_shaders)
    add_dependencies(synbench_gpu_backends synbench_gpu_shaders)
    target_include_directories(synbench_gpu_backends PRIVATE "${SYNBENCH_GPU_SHADER_DIR}")
endif()

# The worker: Qt only for the window, the screen and the event loop (QtGui,
# not Quick), the default C runtime (unlike synbench-cpucrypto).
add_executable(synbench-gpu
    main.cpp
    emit.h emit.cpp
    worker_window.h worker_window.cpp
    qt_glue.h qt_glue.cpp
    worker.qrc
)
# Vulkan::Headers (through volk) for qt_glue's QVulkanInstance.
target_link_libraries(synbench-gpu PRIVATE synbench_gpu_backends synbench_gpu_model Qt6::Gui)
set_target_properties(synbench-gpu PROPERTIES AUTORCC ON)
if(MSVC AND NOT SYNBENCH_DEV_HOOKS)
    # The loader resolves the worker's own imports (Qt, the VC++ runtime) from
    # System32 and its folder only (packaging/checks/pe-loadflags.py checks
    # it); SetDefaultDllDirectories in main() covers the later loads. PACKAGED
    # builds only, as SynBench.exe: a development build finds Qt through PATH.
    target_link_options(synbench-gpu PRIVATE /DEPENDENTLOADFLAG:0xA00)
endif()

# Next to SynBench.exe on Windows (it uses the app's Qt DLLs and plugins);
# usr/libexec in the AppDir, beside synbench-helper, with its own qt.conf
# (packaging/linux/make-appdir.py) and the bundled Qt through the rpath.
if(WIN32)
    install(TARGETS synbench-gpu RUNTIME DESTINATION .)
else()
    include(GNUInstallDirs)
    set_target_properties(synbench-gpu PROPERTIES INSTALL_RPATH "\$ORIGIN/../lib")
    install(TARGETS synbench-gpu RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
endif()
