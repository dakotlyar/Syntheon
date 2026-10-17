# SPDX-License-Identifier: GPL-3.0-or-later
# The installed OpenSSL says the build has the hardened options of P17 (no
# config autoload, no DSO / engine loading), still has its assembly, was
# compiled optimised, and points its compiled-in directories (OPENSSLDIR,
# MODULESDIR, ENGINESDIR) at the fixed dummy, not at the build machine.
#   cmake -DROOT=<SYNBENCH_OPENSSL_ROOT> -DLIBRARY=<libcrypto> -DBAKED=<dummy prefix> -P check-config.cmake
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

if(LIBRARY)
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
message(STATUS "OpenSSL configuration: hardened, assembly on, optimised, directories at ${BAKED}")
