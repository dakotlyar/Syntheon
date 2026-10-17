# SPDX-License-Identifier: GPL-3.0-or-later
# The installed OpenSSL says the build has the hardened options of P17 (no
# config autoload, no DSO / engine loading), still has its assembly, was
# compiled optimised, and points its compiled-in directories (OPENSSLDIR,
# MODULESDIR, ENGINESDIR) at the fixed dummy, not at the build machine.
#   cmake -DROOT=<SYNBENCH_OPENSSL_ROOT> -DLIBRARY=<libcrypto> -DBAKED=<dummy prefix> [-DWINCTX=<ctx>] -P check-config.cmake
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

if(EXISTS "${ROOT}/synbench-openssl-cflags.txt")
    file(READ "${ROOT}/synbench-openssl-cflags.txt" cflags)
    if(NOT cflags MATCHES "(^| )(-O3|/O2)( |\n|$)")
        list(APPEND problems "built without -O3 / /O2: ${cflags}")
    endif()
else()
    list(APPEND problems "no synbench-openssl-cflags.txt in ${ROOT}")
endif()

if(LIBRARY AND WINCTX)
    # Windows: the directories come from the registry context, not the build.
    # 1. The define reached OpenSSL's makefile (CPPFLAGS, recorded at configure).
    if(NOT cflags MATCHES "[= ][-/]DOSSL_WINCTX=${WINCTX}( |\n|$)")
        list(APPEND problems "OpenSSL's makefile CPPFLAGS lack -DOSSL_WINCTX=${WINCTX}")
    endif()
    # 2. defaults.c was compiled with it: its registry key, a wide (UTF-16LE)
    #    string because OpenSSL's Windows builds define UNICODE. Not
    #    OpenSSL_version(OPENSSL_WINCTX): in 3.5.9 cversion.c stringizes the
    #    macro's name (TOSTR without a second expansion level), so that string
    #    reads OSSL_WINCTX: "OSSL_WINCTX" whatever the context is.
    file(STRINGS "${LIBRARY}" key ENCODING UTF-16LE REGEX "WOW6432Node\\\\OpenSSL-[0-9]+\\.[0-9]+-${WINCTX}$")
    if(NOT key)
        file(STRINGS "${LIBRARY}" seen REGEX "OSSL_WINCTX|OpenSSL-[0-9]+\\.[0-9]+-")
        file(STRINGS "${LIBRARY}" seen16 ENCODING UTF-16LE REGEX "OpenSSL-[0-9]+\\.[0-9]+-")
        list(APPEND problems "${LIBRARY} has no registry key ...\\OpenSSL-<major>.<minor>-${WINCTX} (OSSL_WINCTX=${WINCTX})"
                             "  ASCII seen: ${seen}" "  UTF-16LE seen: ${seen16}")
    else()
        message(STATUS "OSSL_WINCTX registry key: ${key}")
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
