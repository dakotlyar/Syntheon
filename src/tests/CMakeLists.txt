# SPDX-License-Identifier: GPL-3.0-or-later
# The built-in tests, in canonical order (P19). Adding a phase's real test means
# swapping its folder's contents; this list and the core don't change (P4).
#
# Convention: each <id>/ folder builds a static library syntheon_test_<id>, and
# its header <id>/<id>_test.h declares sb::tests::<Id>Test (ai -> AiTest).
# builtin_tests.cpp is generated from this list; there are no self-registering
# static initialisers (linkers drop them from static libs, and their order is
# undefined).
set(SYNTHEON_TESTS cpu ram storage gpu ai npu battery)

add_subdirectory(common)

set(SYNTHEON_TEST_INCLUDES "")
set(SYNTHEON_TEST_REGISTRATIONS "")
set(_test_libs "")
foreach(id IN LISTS SYNTHEON_TESTS)
    add_subdirectory(${id})
    string(SUBSTRING "${id}" 0 1 _first)
    string(TOUPPER "${_first}" _first)
    string(SUBSTRING "${id}" 1 -1 _rest)
    string(APPEND SYNTHEON_TEST_INCLUDES "#include \"tests/${id}/${id}_test.h\"\n")
    string(APPEND SYNTHEON_TEST_REGISTRATIONS
        "    registry.add(std::make_unique<tests::${_first}${_rest}Test>());\n")
    list(APPEND _test_libs syntheon_test_${id})
endforeach()

configure_file(builtin_tests.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/builtin_tests.cpp" @ONLY)

add_library(syntheon_builtin_tests STATIC
    builtin_tests.h
    "${CMAKE_CURRENT_BINARY_DIR}/builtin_tests.cpp"
)
target_link_libraries(syntheon_builtin_tests PUBLIC syntheon_core PRIVATE ${_test_libs})
