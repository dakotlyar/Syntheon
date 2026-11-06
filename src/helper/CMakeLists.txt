# SPDX-License-Identifier: GPL-3.0-or-later
# The Linux root helper (§6). No Qt: libc/libstdc++ and the protocol codec only.
add_library(syntheon_helper_core STATIC helper_core.h helper_core.cpp)
target_include_directories(syntheon_helper_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(syntheon_helper_core PUBLIC syntheon_protocol)

add_executable(syntheon-helper main.cpp)
target_link_libraries(syntheon-helper PRIVATE syntheon_helper_core syntheon_version)
set_target_properties(syntheon-helper PROPERTIES INSTALL_RPATH "")  # root runs this: no library search path of its own
# Runs as root: no C++ runtime of its own to find on the user's system.
if(NOT MSVC)
    target_link_options(syntheon-helper PRIVATE -static-libstdc++ -static-libgcc)
endif()
