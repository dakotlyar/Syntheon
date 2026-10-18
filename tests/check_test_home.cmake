# SPDX-License-Identifier: GPL-3.0-or-later
#
# The CTest fixture behind "no test reaches the real history or settings".
# Every Qt test runs with HOME (and XDG_DATA_HOME / XDG_CONFIG_HOME) pointed
# at <build>/test-home, so on Unix even a broken test can only reach that.
#   MODE=setup  recreate <build>/test-home before the tests, with its XDG
#               cache/state folders and the runtime folder (RUNTIME_DIR:
#               <test-home>/run, or <XDG_RUNTIME_DIR or /tmp>/
#               synbench-test-run-<hash> for a deep build tree) private, 0700,
#               as XDG_RUNTIME_DIR must be. A runtime folder outside the home
#               must not be a symlink and must be owned by the current user,
#               before and after it is recreated.
#   MODE=check  after them: fail if anything named SynBench/synbench (any
#               case) appeared outside .qttest, Qt's test-mode root. That is
#               a test (or an app it launched) resolving a real default path.
#               The runtime folder is not checked: its single-instance
#               sockets and locks belong there.
# Usage: cmake -DHOME_DIR=<dir> -DMODE=setup|check -P check_test_home.cmake
cmake_minimum_required(VERSION 3.25)
if(NOT HOME_DIR OR NOT HOME_DIR MATCHES "/test-home$")
    message(FATAL_ERROR "HOME_DIR must be a <build>/test-home folder, got '${HOME_DIR}'")
endif()
if(MODE STREQUAL "setup")
    if(NOT RUNTIME_DIR STREQUAL "${HOME_DIR}/run" AND NOT RUNTIME_DIR MATCHES "^/.+/synbench-test-run-[0-9a-f]+$")
        message(FATAL_ERROR "RUNTIME_DIR must be <test-home>/run or <dir>/synbench-test-run-<hash>, got '${RUNTIME_DIR}'")
    endif()
    set(outside FALSE)
    if(NOT RUNTIME_DIR STREQUAL "${HOME_DIR}/run")
        set(outside TRUE)
    endif()
    # A shared parent (/tmp) could hold someone else's folder or a planted
    # link under the predictable name: refuse rather than use it.
    function(check_runtime_dir when)
        if(NOT EXISTS "${RUNTIME_DIR}" AND NOT IS_SYMLINK "${RUNTIME_DIR}")
            return()
        endif()
        if(IS_SYMLINK "${RUNTIME_DIR}")
            message(FATAL_ERROR "runtime folder ${RUNTIME_DIR} is a symlink (${when}); refusing it")
        endif()
        execute_process(COMMAND stat -c %u "${RUNTIME_DIR}" OUTPUT_VARIABLE owner OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE r1)
        execute_process(COMMAND id -u OUTPUT_VARIABLE me OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE r2)
        if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT owner STREQUAL me)
            message(FATAL_ERROR "runtime folder ${RUNTIME_DIR} is owned by uid '${owner}', not '${me}' (${when}); refusing it")
        endif()
    endfunction()
    if(outside)
        check_runtime_dir("before setup")
    endif()
    file(REMOVE_RECURSE "${HOME_DIR}" "${RUNTIME_DIR}")
    foreach(d "${HOME_DIR}" "${HOME_DIR}/.cache" "${HOME_DIR}/.local/state" "${RUNTIME_DIR}")
        file(MAKE_DIRECTORY "${d}")
        file(CHMOD "${d}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    endforeach()
    if(outside)
        check_runtime_dir("after setup")
    endif()
elseif(MODE STREQUAL "check")
    file(GLOB_RECURSE found LIST_DIRECTORIES true RELATIVE "${HOME_DIR}" "${HOME_DIR}/*")
    set(bad "")
    foreach(f IN LISTS found)
        string(TOLOWER "${f}" lower)
        if(lower MATCHES "synbench" AND NOT f MATCHES "^\\.qttest(/|$)" AND NOT f MATCHES "^scratch(/|$)"
           AND NOT f MATCHES "^run(/|$)")
            list(APPEND bad "${f}")
        endif()
    endforeach()
    if(bad)
        list(JOIN bad "\n  " shown)
        message(FATAL_ERROR "A test resolved a real (non-test-mode) SynBench path; created under the test HOME:\n  ${shown}")
    endif()
    message(STATUS "test HOME clean: nothing SynBench outside .qttest, scratch and run")
else()
    message(FATAL_ERROR "MODE must be setup or check")
endif()
