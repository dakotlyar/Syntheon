# SPDX-License-Identifier: GPL-3.0-or-later
# Helper tests (Linux). Nothing here runs as root.
add_executable(synbench-helper-test test_helper_main.cpp)
target_link_libraries(synbench-helper-test PRIVATE synbench_helper_core)

synbench_add_unit_test(tst_helper_core SOURCES tst_helper_core.cpp LIBS synbench_helper_core Qt6::Core)
synbench_add_unit_test(tst_helper_client SOURCES tst_helper_client.cpp LIBS synbench_privileged synbench_sysinfo)
add_dependencies(tst_helper_core synbench-helper)
add_dependencies(tst_helper_client synbench-helper synbench-helper-test)
foreach(t tst_helper_core tst_helper_client)
    target_compile_definitions(${t} PRIVATE
        SB_HELPER_PATH="$<TARGET_FILE:synbench-helper>"
        SB_TEST_HELPER_PATH="$<TARGET_FILE:synbench-helper-test>")
endforeach()

synbench_add_unit_test(tst_app_controller SOURCES tst_app_controller.cpp LIBS synbench_appcore)
add_dependencies(tst_app_controller synbench-helper-test)
target_compile_definitions(tst_app_controller PRIVATE SB_TEST_HELPER_PATH="$<TARGET_FILE:synbench-helper-test>")
