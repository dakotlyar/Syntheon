# SPDX-License-Identifier: GPL-3.0-or-later

# The wire protocol, shared with the root helper: plain C++, no Qt (§1).
add_library(syntheon_protocol STATIC protocol/protocol.h protocol/protocol.cpp)
target_include_directories(syntheon_protocol PUBLIC "${PROJECT_SOURCE_DIR}/src")

# Client side: the PrivilegedOps interface and its platform backends (QtCore).
add_library(syntheon_privileged STATIC privileged_ops.h privileged_ops.cpp)
if(WIN32)
    target_sources(syntheon_privileged PRIVATE win_inproc/windows_inprocess_ops.h win_inproc/windows_inprocess_ops.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(syntheon_privileged PRIVATE linux_client/linux_helper_client.h linux_client/linux_helper_client.cpp)
    set_target_properties(syntheon_privileged PROPERTIES AUTOMOC ON)
endif()
target_include_directories(syntheon_privileged PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(syntheon_privileged PUBLIC Qt6::Core syntheon_protocol PRIVATE syntheon_version)
