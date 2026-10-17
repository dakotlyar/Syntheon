# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 REQUIRED COMPONENTS Test)

# synbench_add_unit_test(<name> SOURCES ... LIBS ...)
# One Qt Test executable per test file, registered with CTest.
#
# Every test gets tests/support/crash_handler.cpp (a crash prints its cause
# instead of leaving an empty log) and writes verbose results to
# <build>/test-logs/<name>.txt as well as stdout, so CI can upload them.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-logs")
function(synbench_add_unit_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    qt_add_executable(${name} ${ARG_SOURCES} "${PROJECT_SOURCE_DIR}/tests/support/crash_handler.cpp")
    target_link_libraries(${name} PRIVATE Qt6::Test ${ARG_LIBS})
    add_test(NAME ${name}
        COMMAND ${name} -v2 -o "${CMAKE_BINARY_DIR}/test-logs/${name}.txt,txt" -o "-,txt")
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

add_subdirectory(unit)
add_subdirectory(qml)

# The shipping-tree check must itself fail on a planted test binary (§10).
if(UNIX)
    add_test(NAME no_test_binaries_selftest
        COMMAND sh -c "d=$(mktemp -d) && mkdir -p \"$d/usr/bin\" && touch \"$d/usr/bin/SynBench\" && bash \"$0\" \"$d\" && touch \"$d/usr/bin/synbench-helper-test\" && ! bash \"$0\" \"$d\" 2>/dev/null; r=$?; rm -rf \"$d\"; exit $r"
                "${PROJECT_SOURCE_DIR}/packaging/checks/no-test-binaries.sh")
endif()

# The qmllint gate (CI's "any QML warning fails") must itself fail on a planted
# warning, with the real qmllint of this Qt.
find_program(SYNBENCH_PYTHON3 python3)
if(UNIX AND SYNBENCH_PYTHON3 AND TARGET Qt6::qmllint)
    add_test(NAME qmllint_gate_selftest
        COMMAND bash "${PROJECT_SOURCE_DIR}/packaging/checks/test-qmllint-gate.sh"
                "$<TARGET_FILE:Qt6::qmllint>" "${PROJECT_SOURCE_DIR}/packaging/checks/qmllint-gate.py")
endif()

# Every tracked path must be checkout-able on Windows.
add_test(NAME windows_safe_paths
    COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/CheckWindowsPaths.cmake")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(helper)
endif()
