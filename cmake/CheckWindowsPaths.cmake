# SPDX-License-Identifier: GPL-3.0-or-later
# Fails if any tracked path can't exist on Windows (a Windows clone would
# fail at checkout): the characters : * ? " < > |, a component ending in a
# dot or space, or a reserved device name. Run as a CTest test:
#   cmake -DSOURCE_DIR=<repo> -P cmake/CheckWindowsPaths.cmake
# Run with cmake -P: the policies of the project's minimum (a script gets
# none from the project, and CMake 3.x would run it with OLD ones).
cmake_minimum_required(VERSION 3.25)
find_package(Git QUIET)
if(NOT GIT_FOUND OR NOT EXISTS "${SOURCE_DIR}/.git")
    message(STATUS "Not a git checkout; skipping the Windows path check")
    return()
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" ls-files
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE files RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "git ls-files failed")
endif()
string(REPLACE "\n" ";" files "${files}")
set(bad "")
foreach(f IN LISTS files)
    if(f STREQUAL "")
        continue()
    endif()
    if(f MATCHES "[:*?\"<>|]")
        list(APPEND bad "${f}")
        continue()
    endif()
    string(REPLACE "/" ";" parts "${f}")
    foreach(p IN LISTS parts)
        string(TOUPPER "${p}" up)
        if(p MATCHES "[. ]$" OR up MATCHES "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\..*)?$")
            list(APPEND bad "${f}")
            break()
        endif()
    endforeach()
endforeach()
if(bad)
    list(JOIN bad "\n  " text)
    message(FATAL_ERROR "Paths a Windows checkout can't hold:\n  ${text}")
endif()
message(STATUS "All tracked paths are valid on Windows")
