# SPDX-License-Identifier: GPL-3.0-or-later
#
# The CTest fixture behind "no test reaches the real history or settings".
# Every Qt test runs with HOME (and the XDG data/config/cache/state homes)
# pointed at <build>/test-home, so on Unix even a broken test can only reach
# that, and with a private runtime folder of its own.
#   MODE=setup  recreate <build>/test-home (its XDG cache/state folders 0700)
#               and create the runtime folder, chosen now, at test time:
#               mktemp -d <parent>/syntheon-test-run-XXXXXX (atomic, random,
#               0700), where <parent> is the caller's own XDG_RUNTIME_DIR
#               (/run/user/<uid>: private, cleared at logout), or /tmp when
#               XDG_RUNTIME_DIR is unset. An XDG_RUNTIME_DIR that is missing,
#               a symlink or another user's (sudo ctest) is refused: nothing is
#               ever created inside another uid's runtime folder. The path
#               goes to <test-home>/runtime-dir, which every test binary reads
#               (tests/support/test_isolation.cpp sets XDG_RUNTIME_DIR from it).
#   MODE=check  after the tests: remove that runtime folder (it must still be
#               ours and not a symlink), then fail if anything named Syntheon
#               (any case) appeared under the test HOME outside .qttest, Qt's
#               test-mode root, and scratch: a test (or an app it launched)
#               resolving a real default path.
# Usage: cmake -DHOME_DIR=<dir> -DMODE=setup|check -P check_test_home.cmake
cmake_minimum_required(VERSION 3.25)
if(NOT HOME_DIR OR NOT HOME_DIR MATCHES "/test-home$")
    message(FATAL_ERROR "HOME_DIR must be a <build>/test-home folder, got '${HOME_DIR}'")
endif()
set(runtime_file "${HOME_DIR}/runtime-dir")

# The uid owning <path> ("" if it can't be read): GNU stat -c %u, or, on
# Darwin/BSD only, stat -f %u (on GNU, -f means file-system status).
execute_process(COMMAND uname -s OUTPUT_VARIABLE kernel OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
function(owner_of path out)
    if(kernel MATCHES "Darwin|BSD")
        set(args -f %u)
    else()
        set(args -c %u)
    endif()
    execute_process(COMMAND stat ${args} "${path}" OUTPUT_VARIABLE o OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE r ERROR_QUIET)
    if(NOT r EQUAL 0 OR NOT o MATCHES "^[0-9]+$")
        set(o "")
    endif()
    set(${out} "${o}" PARENT_SCOPE)
endfunction()
execute_process(COMMAND id -u OUTPUT_VARIABLE me OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE r)
if(NOT r EQUAL 0 OR me STREQUAL "")
    message(FATAL_ERROR "can't tell the current uid (id -u failed)")
endif()

# FATAL_ERROR unless <path> is a real directory (not a symlink) owned by us.
function(require_ours path what)
    if(IS_SYMLINK "${path}")
        message(FATAL_ERROR "${what} ${path} is a symlink; refusing it")
    endif()
    if(NOT IS_DIRECTORY "${path}")
        message(FATAL_ERROR "${what} ${path} doesn't exist or isn't a folder")
    endif()
    owner_of("${path}" owner)
    if(owner STREQUAL "")
        message(FATAL_ERROR "${what} ${path}: can't read the owner; refusing it")
    endif()
    if(NOT owner STREQUAL me)
        message(FATAL_ERROR "${what} ${path} is owned by uid '${owner}', not yours ('${me}'); refusing it")
    endif()
endfunction()

# The runtime folder recorded by a setup, removed if it is still ours.
function(remove_recorded_runtime)
    if(NOT EXISTS "${runtime_file}")
        return()
    endif()
    file(READ "${runtime_file}" dir)
    string(STRIP "${dir}" dir)
    if(NOT dir MATCHES "^/.+/syntheon-test-run-[A-Za-z0-9]+$")
        message(FATAL_ERROR "${runtime_file} names '${dir}', not a syntheon-test-run folder")
    endif()
    if(EXISTS "${dir}" OR IS_SYMLINK "${dir}")
        require_ours("${dir}" "runtime folder")
        file(REMOVE_RECURSE "${dir}")
    endif()
    # Gone with its folder: a later run without the setup fixture then gets
    # test_isolation's "run the tests through ctest", not a deleted path.
    file(REMOVE "${runtime_file}")
endfunction()

if(MODE STREQUAL "setup")
    set(parent "$ENV{XDG_RUNTIME_DIR}")
    if(parent STREQUAL "")
        set(parent "/tmp")
    else()
        if(IS_SYMLINK "${parent}" OR NOT IS_DIRECTORY "${parent}")
            message(FATAL_ERROR "XDG_RUNTIME_DIR ${parent} doesn't exist or is a symlink; re-run in a user session")
        endif()
        owner_of("${parent}" owner)
        if(owner STREQUAL "")
            message(FATAL_ERROR "XDG_RUNTIME_DIR ${parent}: can't read the owner; re-run in a user session")
        endif()
        if(NOT owner STREQUAL me)
            message(FATAL_ERROR "XDG_RUNTIME_DIR ${parent} isn't yours (uid '${owner}', you are '${me}'); "
                                "re-run in your own user session, not under sudo")
        endif()
    endif()
    remove_recorded_runtime()  # a previous run that never reached its check
    file(REMOVE_RECURSE "${HOME_DIR}")
    foreach(d "${HOME_DIR}" "${HOME_DIR}/.cache" "${HOME_DIR}/.local/state")
        file(MAKE_DIRECTORY "${d}")
        file(CHMOD "${d}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    endforeach()
    execute_process(COMMAND mktemp -d "${parent}/syntheon-test-run-XXXXXX"
                    OUTPUT_VARIABLE runtime OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE r)
    if(NOT r EQUAL 0 OR NOT runtime MATCHES "^/.+/syntheon-test-run-[A-Za-z0-9]+$")
        message(FATAL_ERROR "mktemp -d in ${parent} failed ('${runtime}')")
    endif()
    require_ours("${runtime}" "runtime folder")
    # A Unix socket path holds 104 bytes (macOS; 108 on Linux), and
    # "<runtime>/syntheon/syntheon-<key>.sock" adds 36.
    string(LENGTH "${runtime}" n)
    if(n GREATER 68)
        file(REMOVE_RECURSE "${runtime}")
        message(FATAL_ERROR "runtime folder ${runtime} is too long for the instance sockets (${n} > 68)")
    endif()
    file(WRITE "${runtime_file}" "${runtime}\n")
    message(STATUS "test HOME ${HOME_DIR}; runtime folder ${runtime}")
elseif(MODE STREQUAL "check")
    remove_recorded_runtime()
    file(GLOB_RECURSE found LIST_DIRECTORIES true RELATIVE "${HOME_DIR}" "${HOME_DIR}/*")
    set(bad "")
    foreach(f IN LISTS found)
        string(TOLOWER "${f}" lower)
        if(lower MATCHES "syntheon" AND NOT f MATCHES "^\\.qttest(/|$)" AND NOT f MATCHES "^scratch(/|$)")
            list(APPEND bad "${f}")
        endif()
    endforeach()
    if(bad)
        list(JOIN bad "\n  " shown)
        message(FATAL_ERROR "A test resolved a real (non-test-mode) Syntheon path; created under the test HOME:\n  ${shown}")
    endif()
    message(STATUS "test HOME clean: nothing Syntheon outside .qttest and scratch")
else()
    message(FATAL_ERROR "MODE must be setup or check")
endif()
