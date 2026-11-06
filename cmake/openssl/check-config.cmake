# SPDX-License-Identifier: GPL-3.0-or-later
# The installed OpenSSL says the build has the hardened options of P17 (no
# config autoload, no DSO / engine loading), still has its assembly, was
# compiled optimised, and points its compiled-in directories (OPENSSLDIR,
# MODULESDIR, ENGINESDIR) at the fixed dummy, not at the build machine.
#   cmake -DROOT=<SYNTHEON_OPENSSL_ROOT> -DLIBRARY=<libcrypto> -DBAKED=<dummy prefix> [-DWINCTX=<ctx>] -P check-config.cmake
# (Windows, WINCTX: the library must carry OSSL_WINCTX instead.)
# Run with cmake -P: the policies of the project's minimum (a script gets
# none from the project, and CMake 3.x would run it with OLD ones).
cmake_minimum_required(VERSION 3.25)
set(CONFIG_H "${ROOT}/include/openssl/configuration.h")
file(READ "${CONFIG_H}" text)
set(problems "")
foreach(def OPENSSL_NO_AUTOLOAD_CONFIG OPENSSL_NO_DSO OPENSSL_NO_ENGINE OPENSSL_NO_DYNAMIC_ENGINE OPENSSL_NO_APPS)
    if(NOT text MATCHES "define ${def}[\r\n]")
        list(APPEND problems "${def} is not defined")
    endif()
endforeach()
if(text MATCHES "define OPENSSL_NO_ASM[\r\n]")
    list(APPEND problems "OPENSSL_NO_ASM is defined: no hardware AES/SHA path")
endif()

if(EXISTS "${ROOT}/syntheon-openssl-cflags.txt")
    file(READ "${ROOT}/syntheon-openssl-cflags.txt" cflags)
    # A flag starts the line's value (CFLAGS=-O3 ...) or follows a space.
    if(NOT cflags MATCHES "(^|[ =])(-O3|/O2)( |\n|$)")
        list(APPEND problems "built without -O3 / /O2: ${cflags}")
    endif()
else()
    list(APPEND problems "no syntheon-openssl-cflags.txt in ${ROOT}")
endif()

if(LIBRARY AND WINCTX)
    # Windows: the directories come from the registry context, not the build.
    # 1. The define reached OpenSSL's makefile (CPPFLAGS, recorded at configure).
    if(NOT cflags MATCHES "[= ][-/]DOSSL_WINCTX=${WINCTX}( |\n|$)")
        list(APPEND problems "OpenSSL's makefile CPPFLAGS lack -DOSSL_WINCTX=${WINCTX}")
    endif()
    # 2. defaults.c was compiled with it: its registry key,
    #    "SOFTWARE\\WOW6432Node\\OpenSSL" "-" MAKESTR(major) "." MAKESTR(minor)
    #    "-" MAKESTR(OSSL_WINCTX) under TEXT(), i.e. one wide (UTF-16LE) literal
    #    because OpenSSL's Windows builds define UNICODE. Searched as bytes at
    #    any offset: file(STRINGS ... ENCODING UTF-16LE) reads 2-byte units
    #    from the file's start and misses a wide string at an odd offset,
    #    which a packed COFF member can put it at. Not OpenSSL_version(
    #    OPENSSL_WINCTX): 3.5.9's cversion.c stringizes the macro's name (TOSTR
    #    without a second expansion level), so it reads OSSL_WINCTX: "OSSL_WINCTX".
    file(STRINGS "${ROOT}/include/openssl/opensslv.h" ver REGEX "define OPENSSL_VERSION_(MAJOR|MINOR) ")
    string(REGEX MATCH "MAJOR +([0-9]+)" _m "${ver}")
    set(major "${CMAKE_MATCH_1}")
    string(REGEX MATCH "MINOR +([0-9]+)" _m "${ver}")
    set(minor "${CMAKE_MATCH_1}")
    set(key "OpenSSL-${major}.${minor}-${WINCTX}")
    set(wide "")
    set(narrow "")
    string(LENGTH "${key}" klen)
    math(EXPR klast "${klen} - 1")
    foreach(i RANGE ${klast})
        string(SUBSTRING "${key}" ${i} 1 ch)
        string(HEX "${ch}" hx)
        string(APPEND wide "${hx}00")
        string(APPEND narrow "${hx}")
    endforeach()
    file(READ "${LIBRARY}" hex HEX)
    # An even index in the hex text = a match at a byte boundary.
    function(find_bytes hex pattern out)
        set(found "")
        set(base 0)
        while(TRUE)
            string(FIND "${hex}" "${pattern}" at)
            if(at EQUAL -1)
                break()
            endif()
            math(EXPR pos "${base} + ${at}")
            math(EXPR odd "${pos} % 2")
            if(odd EQUAL 0)
                math(EXPR byte "${pos} / 2")
                list(APPEND found ${byte})
            endif()
            math(EXPR skip "${at} + 1")
            string(SUBSTRING "${hex}" ${skip} -1 hex)
            math(EXPR base "${base} + ${skip}")
        endwhile()
        set(${out} "${found}" PARENT_SCOPE)
    endfunction()
    find_bytes("${hex}" "${wide}" at16)
    if(at16)
        list(GET at16 0 first)
        math(EXPR parity "${first} % 2")
        if(parity)
            set(parity "an odd")
        else()
            set(parity "an even")
        endif()
        message(STATUS "OSSL_WINCTX registry key ${key} found as UTF-16LE at byte offset(s) ${at16} "
                       "(the first at ${parity} offset)")
    else()
        find_bytes("${hex}" "${narrow}" at8)
        # ossl_get_wininstallcontext() in the same defaults.c returns the
        # context as a narrow string, or "Undefined" when OSSL_WINCTX wasn't
        # defined there.
        file(STRINGS "${LIBRARY}" seen REGEX "OSSL_WINCTX|compiler: |^${WINCTX}$|^Undefined$")
        set(recorded "")
        if(EXISTS "${ROOT}/syntheon-openssl-cflags.txt")
            file(READ "${ROOT}/syntheon-openssl-cflags.txt" recorded)
        endif()
        list(APPEND problems "${LIBRARY} has no registry key ...\\${key} as UTF-16LE at any offset (OSSL_WINCTX=${WINCTX})"
                             "  as ASCII at byte offsets: ${at8}"
                             "  ASCII strings seen: ${seen}"
                             "  OpenSSL's makefile (recorded at configure):\n${recorded}")
    endif()
elseif(LIBRARY)
    file(STRINGS "${LIBRARY}" dirs REGEX "OPENSSLDIR: \"")
    if(NOT dirs)
        list(APPEND problems "no OPENSSLDIR string in ${LIBRARY}")
    endif()
    foreach(d IN LISTS dirs)
        # Windows builds spell the path with backslashes.
        string(REPLACE "\\" "/" d "${d}")
        string(REPLACE "//" "/" d "${d}")
        string(FIND "${d}" "${BAKED}" at)
        if(at EQUAL -1)
            list(APPEND problems "OPENSSLDIR isn't the fixed dummy ${BAKED}: ${d}")
        endif()
    endforeach()
endif()

if(problems)
    list(JOIN problems "\n  " problems)
    message(FATAL_ERROR "${ROOT}:\n  ${problems}")
endif()
if(WINCTX)
    message(STATUS "OpenSSL configuration: hardened, assembly on, optimised, directories from registry context ${WINCTX}")
else()
    message(STATUS "OpenSSL configuration: hardened, assembly on, optimised, directories at ${BAKED}")
endif()
