# SPDX-License-Identifier: GPL-3.0-or-later
# Every SB_NO_VECTOR is followed by a classic for / while / do loop
# (kernel_common.h): it is MSVC's `#pragma loop(no_vector)`, which takes no
# range-based for (error C3925; CI is the only MSVC here), with or without a
# C++20 init-statement.
#   cmake [-DSOURCE_DIR=<dir>] [-DFILES=<file>|<file>...] -P CheckNoVectorPragmas.cmake
# SOURCE_DIR: every .cpp / .h / .inc under <dir>/src. FILES: those files too.
# The loop may follow the macro on the same line or on the next one.
# Run with cmake -P: the policies of the project's minimum (a script gets
# none from the project, and CMake 3.x would run it with OLD ones).
cmake_minimum_required(VERSION 3.25)
set(files "")
if(SOURCE_DIR)
    file(GLOB_RECURSE files "${SOURCE_DIR}/src/*.cpp" "${SOURCE_DIR}/src/*.h" "${SOURCE_DIR}/src/*.inc")
endif()
if(FILES)
    string(REPLACE "|" ";" more "${FILES}")
    list(APPEND files ${more})
endif()
set(problems "")
set(total 0)
foreach(f IN LISTS files)
    # The whole text, not file(STRINGS): CMake lists split lines at ';' and
    # merge them at unbalanced brackets, so both are replaced first.
    file(READ "${f}" text)
    string(REPLACE ";" "<semicolon>" text "${text}")
    string(REPLACE "[" "<lbracket>" text "${text}")
    string(REPLACE "]" "<rbracket>" text "${text}")
    # A use is the macro at the start of a line (not its #define, not a
    # comment), with the rest of that line and the next one.
    string(REGEX MATCHALL "\n[ \t]*SB_NO_VECTOR[^\n]*\n[^\n]*" sites "\n${text}")
    foreach(site IN LISTS sites)
        string(REGEX MATCH "^\n[ \t]*SB_NO_VECTOR([^\n]*)\n([^\n]*)$" _ "${site}")
        set(rest "${CMAKE_MATCH_1}")
        set(next "${CMAKE_MATCH_2}")
        if(rest MATCHES "^[ \t]*$")
            set(loop "${next}")
        else()
            set(loop "${rest}")
        endif()
        math(EXPR total "${total} + 1")
        # A classic for has two ';' in its header before any single ':' (a
        # range-based for has a ':' after at most one ';', its init-statement).
        if(NOT loop MATCHES "^[ \t]*(for \\(([^:]|::)*<semicolon>([^:]|::)*<semicolon>|while[ \t]*\\(|do([ \t{]|$))")
            string(REPLACE "<semicolon>" ";" shown "${loop}")
            # A string, not a list: the loop text has ';' in it.
            string(APPEND problems "\n  ${f}: SB_NO_VECTOR is followed by '${shown}', not a classic for / while / do loop")
        endif()
    endforeach()
endforeach()
list(LENGTH files nfiles)
message(STATUS "${total} SB_NO_VECTOR site(s) in ${nfiles} file(s)")
if(problems)
    message(FATAL_ERROR "MSVC's loop pragma can't go there:${problems}")
endif()
