# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 REQUIRED COMPONENTS Test)

# synbench_add_unit_test(<name> SOURCES ... LIBS ...)
# One Qt Test executable per test file, registered with CTest.
function(synbench_add_unit_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    qt_add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE Qt6::Test ${ARG_LIBS})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

add_subdirectory(unit)

# Every tracked path must be checkout-able on Windows.
add_test(NAME windows_safe_paths
    COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/CheckWindowsPaths.cmake")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(helper)
endif()
