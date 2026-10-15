# SPDX-License-Identifier: GPL-3.0-or-later
include(FetchContent)
# Vulkan probe: headers only, pinned (§8). The loader is opened at run time.
FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.304.tar.gz
    URL_HASH SHA256=5e1a06b3f27e7581b55d1dea176fd97ee0a2f299406db2f437c1d2f297ceba5b
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(VulkanHeaders)

find_package(Qt6 REQUIRED COMPONENTS Gui)

qt_add_library(synbench_sysinfo STATIC
    system_info.h system_info.cpp
    collector.h collector.cpp
    parsers/text_parsers.h parsers/text_parsers.cpp
    parsers/smbios.h parsers/smbios.cpp
    parsers/windows_parsers.h parsers/windows_parsers.cpp
    common/x86_cpu.h common/x86_cpu.cpp
    common/vulkan_probe.h common/vulkan_probe.cpp
    common/opengl_probe.h common/opengl_probe.cpp
    common/app_info.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(synbench_sysinfo PRIVATE
        linux/sysfs_readers.h linux/sysfs_readers.cpp
        linux/collect_linux.cpp
    )
elseif(WIN32)
    target_sources(synbench_sysinfo PRIVATE win/collect_windows.cpp)
    target_link_libraries(synbench_sysinfo PRIVATE setupapi)
endif()

# QtGui only for the OpenGL probe (common/opengl_probe.cpp, §1).
target_link_libraries(synbench_sysinfo
    PUBLIC synbench_core
    PRIVATE Qt6::Gui Vulkan::Headers synbench_version)
