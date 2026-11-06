# SPDX-License-Identifier: GPL-3.0-or-later
# syntheon-gpu, the GPU test's worker (Phase 4a design §1, §2): one process per
# API, so a driver crash or a GPU hang never takes Syntheon down.
#
# syntheon_gpu_backends: Qt-free (P2): the backend interface, the runner, the
# signature and the backends themselves (null; Vulkan from task 10; D3D12,
# D3D11 and OpenGL from tasks 18, 20 and 21; Direct2D from task 27).
add_library(syntheon_gpu_backends STATIC
    backend.h present_constants.h scene_constants.h scene_constants.cpp
    runner.h runner.cpp
    signature.h signature.cpp
    null/null_backend.h null/null_backend.cpp
    vulkan/vk_context.h vulkan/vk_context.cpp
    vulkan/vk_probe.h vulkan/vk_probe.cpp
    vulkan/layer_manifests.h vulkan/layer_manifests.cpp
    vulkan/vk_resources.h vulkan/vk_resources.cpp
    vulkan/vk_swapchain.h vulkan/vk_swapchain.cpp
    vulkan/vk_backend.h vulkan/vk_backend.cpp
    vulkan/vk_workload_state.h vulkan/vk_workloads.cpp
    gl/gl_backend.h gl/gl_backend.cpp
    gl/gl_workload_state.h gl/gl_workloads.cpp
)
if(WIN32)
    # Layer detection: EnumProcessModules (psapi) and the ImplicitLayers registry values (advapi32).
    target_link_libraries(syntheon_gpu_backends PRIVATE psapi advapi32)
    # System32's vulkan-1.dll for volk (P-WIN: Windows-only code in its own file).
    target_sources(syntheon_gpu_backends PRIVATE vulkan/vk_loader.h vulkan/vk_loader_win.cpp)
    # Task 18: DXGI (shared by DX12, DX11 and D2D) and the D3D12 backend, with
    # the import libraries of the in-box DLLs (§13: no Agility SDK).
    target_sources(syntheon_gpu_backends PRIVATE
        dxgi/dxgi_swapchain.h dxgi/dxgi_swapchain.cpp
        dxgi/dxgi_probe.h dxgi/dxgi_probe.cpp
        d3d12/d12_context.h d3d12/d12_context.cpp
        d3d12/d12_resources.h d3d12/d12_resources.cpp
        d3d12/d12_backend.h d3d12/d12_backend.cpp
        d3d12/d12_workload_state.h d3d12/d12_workloads.cpp)
    target_link_libraries(syntheon_gpu_backends PRIVATE d3d12 dxgi dxguid)
    # Task 20: the D3D11 backend (the same DXGI swapchain; FXC's DXBC).
    target_sources(syntheon_gpu_backends PRIVATE
        d3d11/d11_backend.h d3d11/d11_backend.cpp
        d3d11/d11_workload_state.h d3d11/d11_workloads.cpp)
    target_link_libraries(syntheon_gpu_backends PRIVATE d3d11)
    # Task 27: the Direct2D native 2D path (a D3D11 device with BGRA support, the same DXGI swapchain, DirectWrite).
    target_sources(syntheon_gpu_backends PRIVATE d2d/d2d_native.h d2d/d2d_native.cpp)
    # MSVC's dwrite_3.h guards IDWriteFactory5, IDWriteInMemoryFontFileLoader and IDWriteFontSetBuilder1 behind
    # NTDDI_VERSION >= NTDDI_WIN10_RS2 (0x0A000003); nothing else in the repo sets the target version. Only this file
    # (the SDK's default may already be newer: the definitions can't lower it below what the headers need).
    set_source_files_properties(d2d/d2d_native.cpp PROPERTIES COMPILE_DEFINITIONS "_WIN32_WINNT=0x0A00;NTDDI_VERSION=0x0A000003")
    target_link_libraries(syntheon_gpu_backends PRIVATE d2d1 dwrite)
endif()
target_link_libraries(syntheon_gpu_backends PUBLIC syntheon_gpu_scene syntheon::volk)
# Task 21: glad's GL 3.3 core loader (the context is the glue's; nothing links libGL).
target_link_libraries(syntheon_gpu_backends PRIVATE syntheon::glad)
set_target_properties(syntheon_gpu_backends PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# The Windows build's DXIL/DXBC includes (worker/shaders/CMakeLists.txt): the
# target, not the generated files, so they are never listed as sources here.
if(TARGET syntheon_gpu_shaders)
    add_dependencies(syntheon_gpu_backends syntheon_gpu_shaders)
    target_include_directories(syntheon_gpu_backends PRIVATE "${SYNTHEON_GPU_SHADER_DIR}")
endif()

# The Qt raster native 2D path (task 26): Qt Gui, so a library of its own (the backends are Qt-free), shared
# by the worker and tst_gpu_native. The font it draws text with is worker.qrc's (attach it to the executable).
add_library(syntheon_gpu_qtraster STATIC qtraster/qt_native.h qtraster/qt_native.cpp)
target_link_libraries(syntheon_gpu_qtraster PUBLIC syntheon_gpu_scene Qt6::Gui)
target_include_directories(syntheon_gpu_qtraster PUBLIC "${PROJECT_SOURCE_DIR}/src")
set_target_properties(syntheon_gpu_qtraster PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# The worker: Qt only for the window, the screen and the event loop (QtGui,
# not Quick), the default C runtime (unlike syntheon-cpucrypto).
add_executable(syntheon-gpu
    main.cpp
    emit.h emit.cpp
    worker_window.h worker_window.cpp
    qt_glue.h qt_glue.cpp
    worker.qrc
)
# Vulkan::Headers (through volk) for qt_glue's QVulkanInstance.
target_link_libraries(syntheon-gpu PRIVATE syntheon_gpu_backends syntheon_gpu_qtraster syntheon_gpu_model Qt6::Gui)
set_target_properties(syntheon-gpu PROPERTIES AUTORCC ON)
if(MSVC AND NOT SYNTHEON_DEV_HOOKS)
    # The loader resolves the worker's own imports (Qt, the VC++ runtime) from
    # System32 and its folder only (packaging/checks/pe-loadflags.py checks
    # it); SetDefaultDllDirectories in main() covers the later loads. PACKAGED
    # builds only, as Syntheon.exe: a development build finds Qt through PATH.
    target_link_options(syntheon-gpu PRIVATE /DEPENDENTLOADFLAG:0xA00)
endif()

# Next to Syntheon.exe on Windows (it uses the app's Qt DLLs and plugins);
# usr/libexec in the AppDir, beside syntheon-helper, with its own qt.conf
# (packaging/linux/make-appdir.py) and the bundled Qt through the rpath.
if(WIN32)
    install(TARGETS syntheon-gpu RUNTIME DESTINATION .)
else()
    include(GNUInstallDirs)
    set_target_properties(syntheon-gpu PROPERTIES INSTALL_RPATH "\$ORIGIN/../lib")
    install(TARGETS syntheon-gpu RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
endif()
