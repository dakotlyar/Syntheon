# SPDX-License-Identifier: GPL-3.0-or-later
# Every SB_NO_VECTOR in a kernel TU is followed by a classic for / while / do
# loop (kernel_common.h): it is MSVC's `#pragma loop(no_vector)`, which a
# range-based for doesn't take (error C3925, and CI is the only MSVC here).
#   cmake -DFILES=<file>|<file>... -P CheckNoVectorPragmas.cmake
# Run with cmake -P: the policies of the project's minimum (a script gets
# none from the project, and CMake 3.x would run it with OLD ones).
cmake_minimum_required(VERSION 3.25)
string(REPLACE "|" ";" files "${FILES}")
set(problems "")
foreach(f IN LISTS files)
    # The whole text, not file(STRINGS): CMake lists split lines at ';' and
    # merge them at unbalanced brackets.
    file(READ "${f}" text)
    # ';' would split the matches below into several list items.
    string(REPLACE ";" "<semicolon>" text "${text}")
    string(REPLACE "[" "<lbracket>" text "${text}")
    string(REGEX MATCHALL "\n[ \t]*SB_NO_VECTOR[ \t]*\n[^\n]*" sites "${text}")
    list(LENGTH sites n)
    foreach(site IN LISTS sites)
        string(REGEX REPLACE "^\n[ \t]*SB_NO_VECTOR[ \t]*\n" "" next "${site}")
        # A classic for: its first ";" comes before any single ":" (a range-based
        # for has a ":" and no ";" in its parentheses).
        if(NOT next MATCHES "^[ \t]*(for \\(([^:]|::)*<semicolon>|while[ \t]*\\(|do([ \t{]|$))")
            list(APPEND problems "${f}: SB_NO_VECTOR is followed by '${next}', not a classic for / while / do loop")
        endif()
    endforeach()
    message(STATUS "${n} SB_NO_VECTOR site(s) in ${f}")
endforeach()
if(problems)
    list(JOIN problems "\n  " problems)
    message(FATAL_ERROR "MSVC's loop pragma can't go there:\n  ${problems}")
endif()
