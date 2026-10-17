# SPDX-License-Identifier: GPL-3.0-or-later
# The golden-table generator (Phase 2 design §6.4): a developer tool, never
# installed or packaged; built with the tests.
add_executable(synbench-cpu-golden main.cpp)
target_link_libraries(synbench-cpu-golden PRIVATE synbench_test_cpu)
