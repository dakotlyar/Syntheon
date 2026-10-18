# SPDX-License-Identifier: GPL-3.0-or-later
#
# The CTest fixture behind "no test reaches the real history or settings".
# Every Qt test runs with HOME (and XDG_DATA_HOME / XDG_CONFIG_HOME) pointed
# at <build>/test-home, so on Unix even a broken test can only reach that.
#   MODE=setup  empty <build>/test-home before the tests
#   MODE=check  after them: fail if anything named SynBench/synbench (any
#               case) appeared outside .qttest, Qt's test-mode root. That is
#               a test (or an app it launched) resolving a real default path.
# Usage: cmake -DHOME_DIR=<dir> -DMODE=setup|check -P check_test_home.cmake
cmake_minimum_required(VERSION 3.25)
if(NOT HOME_DIR OR NOT HOME_DIR MATCHES "/test-home$")
    message(FATAL_ERROR "HOME_DIR must be a <build>/test-home folder, got '${HOME_DIR}'")
endif()
if(MODE STREQUAL "setup")
    file(REMOVE_RECURSE "${HOME_DIR}")
    file(MAKE_DIRECTORY "${HOME_DIR}")
elseif(MODE STREQUAL "check")
    file(GLOB_RECURSE found LIST_DIRECTORIES true RELATIVE "${HOME_DIR}" "${HOME_DIR}/*")
    set(bad "")
    foreach(f IN LISTS found)
        string(TOLOWER "${f}" lower)
        if(lower MATCHES "synbench" AND NOT f MATCHES "^\\.qttest(/|$)" AND NOT f MATCHES "^scratch(/|$)")
            list(APPEND bad "${f}")
        endif()
    endforeach()
    if(bad)
        list(JOIN bad "\n  " shown)
        message(FATAL_ERROR "A test resolved a real (non-test-mode) SynBench path; created under the test HOME:\n  ${shown}")
    endif()
    message(STATUS "test HOME clean: nothing SynBench outside .qttest and scratch")
else()
    message(FATAL_ERROR "MODE must be setup or check")
endif()
