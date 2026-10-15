# SPDX-License-Identifier: GPL-3.0-or-later
# The Linux root helper (§6). No Qt: libc/libstdc++ and the protocol codec only.
add_library(synbench_helper_core STATIC helper_core.h helper_core.cpp)
target_include_directories(synbench_helper_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(synbench_helper_core PUBLIC synbench_protocol)

add_executable(synbench-helper main.cpp)
target_link_libraries(synbench-helper PRIVATE synbench_helper_core synbench_version)
