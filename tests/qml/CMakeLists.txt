# SPDX-License-Identifier: GPL-3.0-or-later
# Qt Quick Test (offscreen): the QML screens and components against real
# controllers on a temporary history. See tests/qml/main.cpp for the setup.
qt_add_executable(tst_qml "${PROJECT_SOURCE_DIR}/tests/support/test_isolation.cpp" main.cpp
                         "${PROJECT_SOURCE_DIR}/tests/support/crash_handler.cpp")
# The test-only singleton Fixtures (module SynBenchTest), declared at build time
# so qmllint can check the test files against it (-I <build>/tests/qml).
qt_add_qml_module(tst_qml
    URI SynBenchTest
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/SynBenchTest"
    SOURCES fixtures.h
)
target_link_libraries(tst_qml PRIVATE
    Qt6::QuickTest Qt6::Quick Qt6::QuickControls2
    synbench_appcore synbench_ui synbench_uiplugin)
target_compile_definitions(tst_qml PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    SB_FAKE_GPUWORKER="$<TARGET_FILE:fake-gpuworker>")
# The GPU test's worker is a fake here (tests/unit/fake_gpuworker.cpp), the same on every OS.
add_dependencies(tst_qml fake-gpuworker)
add_test(NAME tst_qml
    COMMAND tst_qml -input "${CMAKE_CURRENT_SOURCE_DIR}" -v2
            -o "${CMAKE_BINARY_DIR}/test-logs/tst_qml.txt,txt" -o "-,txt")
synbench_isolate_test(tst_qml)
