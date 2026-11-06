# SPDX-License-Identifier: GPL-3.0-or-later
# Fails if any tracked path can't exist on Windows (a Windows clone would
# fail at checkout): the characters : * ? " < > |, a component ending in a
# dot or space, a reserved device name, or a path over MAX_LENGTH (200)
# characters. Windows' MAX_PATH is 260 with the terminator, and a CI runner
# checks out under D:\a\Syntheon\Syntheon\ (23); 200 leaves room for a
# deeper clone and for the build tools' own temporary names. Run as a CTest
# test:
#   cmake -DSOURCE_DIR=<repo> -P cmake/CheckWindowsPaths.cmake
# or on a given list instead of the tracked files (the planted test):
#   cmake "-DFILES=a;b" -P cmake/CheckWindowsPaths.cmake
# Run with cmake -P: the policies of the project's minimum (a script gets
# none from the project, and CMake 3.x would run it with OLD ones).
cmake_minimum_required(VERSION 3.25)
if(NOT DEFINED MAX_LENGTH)
    set(MAX_LENGTH 200)
endif()
if(DEFINED FILES)
    set(files "${FILES}")
else()
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
endif()
set(bad "")
set(long "")
set(longest 0)
foreach(f IN LISTS files)
    if(f STREQUAL "")
        continue()
    endif()
    string(LENGTH "${f}" n)
    if(n GREATER longest)
        set(longest ${n})
    endif()
    if(n GREATER MAX_LENGTH)
        list(APPEND long "${n}: ${f}")
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
if(long)
    list(JOIN long "\n  " text)
    message(FATAL_ERROR "Paths over ${MAX_LENGTH} characters (a Windows checkout fails with \"Filename too long\"):\n  ${text}")
endif()
message(STATUS "All tracked paths are valid on Windows (the longest is ${longest} characters, the limit ${MAX_LENGTH})")
