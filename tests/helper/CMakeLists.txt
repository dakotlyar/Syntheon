# SPDX-License-Identifier: GPL-3.0-or-later
# Helper tests (Linux). Nothing here runs as root.
add_executable(syntheon-helper-test test_helper_main.cpp)
target_link_libraries(syntheon-helper-test PRIVATE syntheon_helper_core)
# AP5-R2-4: never next to the shipping binaries in <build>/bin.
set_target_properties(syntheon-helper-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-tools")

syntheon_add_unit_test(tst_helper_core SOURCES tst_helper_core.cpp LIBS syntheon_helper_core Qt6::Core)
syntheon_add_unit_test(tst_helper_client SOURCES tst_helper_client.cpp LIBS syntheon_privileged syntheon_sysinfo)
add_dependencies(tst_helper_core syntheon-helper)
add_dependencies(tst_helper_client syntheon-helper syntheon-helper-test)
foreach(t tst_helper_core tst_helper_client)
    target_compile_definitions(${t} PRIVATE
        SB_HELPER_PATH="$<TARGET_FILE:syntheon-helper>"
        SB_TEST_HELPER_PATH="$<TARGET_FILE:syntheon-helper-test>")
endforeach()

syntheon_add_unit_test(tst_app_controller SOURCES tst_app_controller.cpp LIBS syntheon_appcore)
add_dependencies(tst_app_controller syntheon-helper-test)
target_compile_definitions(tst_app_controller PRIVATE SB_TEST_HELPER_PATH="$<TARGET_FILE:syntheon-helper-test>")
