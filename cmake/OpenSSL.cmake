# SPDX-License-Identifier: GPL-3.0-or-later
# OpenSSL 3.5.9 LTS, libcrypto only, static (Phase 2 design §11, D4, D5, P16,
# P17). OpenSSL builds with its own Perl Configure + make/nmake, so CMake
# drives it as an ExternalProject; the result is the imported target
# synbench::crypto, for the encryption worker (synbench-cpucrypto, task 11).
#
# Pin: openssl-3.5.9.tar.gz, SHA-256 603f5602..., verified 2026-10-16 against
# the OpenSSL release signature (.asc; signing subkey C46E D3F2 ... CCE7 1CB2
# of the OpenSSL key B146 647E 45A7 B339 47AB 226B 2A2C 87D1 6169 2D40, the key
# in doc/fingerprints.txt and on openssl-library.org), the release's .sha256
# and GitHub's asset digest.
#
# Targets (§11.2):
#   Linux x64        linux-x86_64
#   Linux ARM64      linux-aarch64, cross (--cross-compile-prefix, the sysroot)
#   Windows x64      VC-WIN64A; NASM assembles the AES-NI / SHA-NI / VAES code
#   Windows ARM64    VC-WIN64-CLANGASM-ARM: cl compiles, clang-cl assembles
#                    (the stock VC-WIN64-ARM has no assembly at all, N4)
# All with the hardened minimal options of P17. Two small Windows patches are
# applied on every target: the armcap override (D5,
# cmake/openssl/patch-win-armcap.cmake) and the uninitialised registry handle
# in get_windows_regdirs (AP-5 L9, cmake/openssl/patch-win-defaults.cmake).
#
# The build is installed into SYNBENCH_OPENSSL_ROOT (default <build>/openssl)
# with a stamp file naming everything that went into it. A configure that
# finds a root whose stamp matches uses it as it is and builds nothing: CI
# caches that folder (the Windows builds take 10-15 minutes, R8). A root with
# a different stamp is rebuilt.
include_guard(GLOBAL)
include(ExternalProject)

set(SYNBENCH_OPENSSL_VERSION 3.5.9)
set(SYNBENCH_OPENSSL_SHA256 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a)
set(SYNBENCH_OPENSSL_ROOT "${CMAKE_BINARY_DIR}/openssl" CACHE PATH
    "Where the OpenSSL build is installed (and found again; CI caches it)")

# --- target and tools -------------------------------------------------------
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _ossl_cpu)
set(_ossl_env "")
set(_ossl_args no-shared no-module no-dso no-engine no-apps no-tests no-docs no-autoload-config)
if(WIN32)
    # A Windows-native Perl (Strawberry, on the runners); MSYS/Git perl gives
    # Unix paths, which OpenSSL's Windows build refuses.
    find_program(SYNBENCH_PERL perl HINTS "C:/Strawberry/perl/bin" NO_CACHE REQUIRED)
    find_program(SYNBENCH_NMAKE nmake REQUIRED)
    set(_ossl_make "${SYNBENCH_NMAKE}")
    # The assemblers are run by name from PATH (OpenSSL's makefile doesn't
    # quote them, and their usual folders have spaces).
    if(_ossl_cpu MATCHES "^(amd64|x86_64|x64)$")
        set(_ossl_target VC-WIN64A)
        find_program(SYNBENCH_NASM nasm NO_CACHE)
        if(NOT SYNBENCH_NASM)
            message(FATAL_ERROR "OpenSSL for Windows x64 needs NASM on PATH (its AES-NI/SHA-NI code is NASM source)")
        endif()
        set(_ossl_tool_version_cmd "${SYNBENCH_NASM}" -v)
    elseif(_ossl_cpu MATCHES "^(arm64|aarch64)$")
        set(_ossl_target VC-WIN64-CLANGASM-ARM)
        find_program(SYNBENCH_CLANG_CL clang-cl NO_CACHE)
        if(NOT SYNBENCH_CLANG_CL)
            message(FATAL_ERROR "OpenSSL for Windows ARM64 needs clang-cl on PATH as its assembler: Visual Studio's "
                                "\"C++ Clang tools\" (<VS>/VC/Tools/Llvm/x64/bin) or LLVM")
        endif()
        set(_ossl_tool_version_cmd "${SYNBENCH_CLANG_CL}" --version)
    else()
        message(FATAL_ERROR "OpenSSL: no Windows target for processor '${CMAKE_SYSTEM_PROCESSOR}'")
    endif()
    set(_ossl_lib "${SYNBENCH_OPENSSL_ROOT}/lib/libcrypto.lib")
    set(_ossl_build_cmd "${_ossl_make}" build_libs)
    set(_ossl_makefile makefile)
    set(_ossl_optimised /O2)
    # M4: the prefix (and OPENSSLDIR, MODULESDIR, ENGINESDIR under it) that is
    # compiled into libcrypto. Nothing is ever loaded from there (P17), and it
    # is a folder a standard user can't create; the real files are installed
    # with DESTDIR and copied into SYNBENCH_OPENSSL_ROOT.
    set(_ossl_baked "C:/Windows/System32/SynBench-no-OpenSSL-files")
    # That path is only safe while Windows is on C:. OSSL_WINCTX (OpenSSL 3.4+,
    # crypto/defaults.c in 3.5.9) makes libcrypto take OPENSSLDIR, ENGINESDIR
    # and MODULESDIR from HKLM\SOFTWARE\WOW6432Node\OpenSSL-3.5-SynBench
    # instead, an admin-only key nobody creates: the directories are then NULL
    # and nothing can be loaded from any folder (NOTES-WINDOWS.md: "returns
    # errors on module/conf load"). The baked path stays as the build default.
    set(SYNBENCH_OPENSSL_WINCTX SynBench)
    list(APPEND _ossl_args "-DOSSL_WINCTX=${SYNBENCH_OPENSSL_WINCTX}")
else()
    find_program(SYNBENCH_PERL perl NO_CACHE REQUIRED)
    find_program(SYNBENCH_MAKE NAMES make gmake NO_CACHE REQUIRED)
    cmake_host_system_information(RESULT _ossl_jobs QUERY NUMBER_OF_LOGICAL_CORES)
    if(_ossl_cpu MATCHES "^(x86_64|amd64)$")
        set(_ossl_target linux-x86_64)
        # The x86-64-v2 baseline (CompilerFlags.cmake): a distro compiler may
        # default to v3. OpenSSL's assembly picks AES-NI, SHA-NI, AVX-512... at
        # run time from CPUID either way.
        list(APPEND _ossl_args -march=x86-64-v2 -mtune=generic)
    elseif(_ossl_cpu MATCHES "^(aarch64|arm64)$")
        set(_ossl_target linux-aarch64)
        list(APPEND _ossl_args -march=armv8-a)
    else()
        message(FATAL_ERROR "OpenSSL: no Linux target for processor '${CMAKE_SYSTEM_PROCESSOR}'")
    endif()
    if(CMAKE_CROSSCOMPILING)
        # Configure takes CC/AR/RANLIB from the prefix; the sysroot reaches
        # the compiler through CFLAGS.
        string(REGEX REPLACE "gcc$" "" _ossl_prefix "${CMAKE_C_COMPILER}")
        list(APPEND _ossl_args "--cross-compile-prefix=${_ossl_prefix}")
        list(APPEND _ossl_env "CC=gcc")
        # A Configure argument, so it is added to OpenSSL's CFLAGS: an
        # environment CFLAGS would replace them (and its -O3).
        if(CMAKE_SYSROOT)
            list(APPEND _ossl_args "--sysroot=${CMAKE_SYSROOT}")
        endif()
    else()
        list(APPEND _ossl_env "CC=${CMAKE_C_COMPILER}")
    endif()
    set(_ossl_lib "${SYNBENCH_OPENSSL_ROOT}/lib/libcrypto.a")
    set(_ossl_build_cmd "${SYNBENCH_MAKE}" -j${_ossl_jobs} build_libs)
    set(_ossl_make "${SYNBENCH_MAKE}")
    set(_ossl_makefile Makefile)
    set(_ossl_optimised -O3)
    # M4 (see the Windows branch): /nonexistent can't be created by a user.
    set(_ossl_baked "/nonexistent/synbench-openssl")
endif()

# --- the stamp: everything that decides the build --------------------------
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/openssl/patch-win-armcap.cmake" _ossl_patch_armcap)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/openssl/patch-win-defaults.cmake" _ossl_patch_defaults)
set(_ossl_patch_sha "${_ossl_patch_armcap} ${_ossl_patch_defaults}")
set(_ossl_tool "")
if(_ossl_tool_version_cmd)
    execute_process(COMMAND ${_ossl_tool_version_cmd} OUTPUT_VARIABLE _ossl_tool ERROR_VARIABLE _ossl_tool_err
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX REPLACE "\n.*" "" _ossl_tool "${_ossl_tool}")
endif()
string(JOIN "\n" _ossl_stamp
    "openssl ${SYNBENCH_OPENSSL_VERSION} ${SYNBENCH_OPENSSL_SHA256}"
    "target ${_ossl_target}"
    "args ${_ossl_args}"
    "env ${_ossl_env}"
    "compiler ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}"
    "assembler ${_ossl_tool}"
    "patch ${_ossl_patch_sha}"
    "baked ${_ossl_baked}")
string(SHA256 _ossl_stamp_id "${_ossl_stamp}")
set(_ossl_stamp_file "${SYNBENCH_OPENSSL_ROOT}/synbench-openssl.stamp")
file(WRITE "${CMAKE_BINARY_DIR}/synbench-openssl.stamp.new" "${_ossl_stamp}\n")

set(_ossl_reuse OFF)
if(EXISTS "${_ossl_stamp_file}" AND EXISTS "${_ossl_lib}")
    file(READ "${_ossl_stamp_file}" _ossl_have)
    string(STRIP "${_ossl_have}" _ossl_have)
    if(_ossl_have STREQUAL _ossl_stamp)
        set(_ossl_reuse ON)
    endif()
endif()
set(SYNBENCH_OPENSSL_STAMP_ID "${_ossl_stamp_id}" CACHE INTERNAL "")

if(_ossl_reuse)
    message(STATUS "OpenSSL ${SYNBENCH_OPENSSL_VERSION} (${_ossl_target}): using the build in ${SYNBENCH_OPENSSL_ROOT}")
    add_custom_target(synbench_openssl)
else()
    message(STATUS "OpenSSL ${SYNBENCH_OPENSSL_VERSION} (${_ossl_target}): building into ${SYNBENCH_OPENSSL_ROOT}")
    # A root with another build's files must not mix with this one. Only a
    # root this file made (it has the stamp, or the "building" marker of a
    # build that hasn't finished yet) or an empty one is cleared.
    set(_ossl_building "${SYNBENCH_OPENSSL_ROOT}/synbench-openssl.building")
    if(EXISTS "${SYNBENCH_OPENSSL_ROOT}")
        file(GLOB _ossl_any "${SYNBENCH_OPENSSL_ROOT}/*")
        if(_ossl_any AND NOT EXISTS "${_ossl_stamp_file}" AND NOT EXISTS "${_ossl_building}")
            message(FATAL_ERROR "SYNBENCH_OPENSSL_ROOT=${SYNBENCH_OPENSSL_ROOT} isn't empty and wasn't made by this build "
                                "(no synbench-openssl.stamp); refusing to clear it")
        endif()
        file(REMOVE_RECURSE "${SYNBENCH_OPENSSL_ROOT}")
    endif()
    file(WRITE "${_ossl_building}" "${_ossl_stamp}\n")
    # The unpacked (and patched) source and the build tree belong to one stamp:
    # another stamp (a new version, options or patch, L3) starts from a fresh
    # extract instead of reusing a tree patched by an older script.
    set(_ossl_prefix "${CMAKE_BINARY_DIR}/_deps/openssl")
    if(EXISTS "${_ossl_prefix}")
        set(_ossl_prev "")
        if(EXISTS "${_ossl_prefix}/synbench-openssl.id")
            file(READ "${_ossl_prefix}/synbench-openssl.id" _ossl_prev)
        endif()
        if(NOT _ossl_prev STREQUAL _ossl_stamp_id)
            file(REMOVE_RECURSE "${_ossl_prefix}")
        endif()
    endif()
    file(WRITE "${_ossl_prefix}/synbench-openssl.id" "${_ossl_stamp_id}")
    # install_dev goes into <build>/stage + the baked prefix (DESTDIR); that
    # tree is copied into the root.
    string(REGEX REPLACE "^[A-Za-z]:" "" _ossl_baked_nodrive "${_ossl_baked}")
    set(_ossl_stage "<BINARY_DIR>/stage")
    ExternalProject_Add(synbench_openssl
        URL "https://github.com/openssl/openssl/releases/download/openssl-${SYNBENCH_OPENSSL_VERSION}/openssl-${SYNBENCH_OPENSSL_VERSION}.tar.gz"
        URL_HASH SHA256=${SYNBENCH_OPENSSL_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${_ossl_prefix}"
        PATCH_COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=<SOURCE_DIR> -P "${CMAKE_CURRENT_LIST_DIR}/openssl/patch-win-armcap.cmake"
            COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=<SOURCE_DIR> -P "${CMAKE_CURRENT_LIST_DIR}/openssl/patch-win-defaults.cmake"
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env ${_ossl_env}
            "${SYNBENCH_PERL}" <SOURCE_DIR>/Configure ${_ossl_target} ${_ossl_args}
            "--prefix=${_ossl_baked}" "--openssldir=${_ossl_baked}/ssl" --libdir=lib
            COMMAND "${CMAKE_COMMAND}" -DMAKEFILE=<BINARY_DIR>/${_ossl_makefile} -DWANT=${_ossl_optimised}
                    -DOUT=<BINARY_DIR>/synbench-openssl-cflags.txt -P "${CMAKE_CURRENT_LIST_DIR}/openssl/check-cflags.cmake"
        BUILD_COMMAND ${_ossl_build_cmd}
        INSTALL_COMMAND ${_ossl_make} install_dev "DESTDIR=${_ossl_stage}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_ossl_stage}${_ossl_baked_nodrive}" "${SYNBENCH_OPENSSL_ROOT}"
            COMMAND "${CMAKE_COMMAND}" -E copy "<BINARY_DIR>/synbench-openssl-cflags.txt" "${SYNBENCH_OPENSSL_ROOT}/"
            COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/synbench-openssl.stamp.new" "${_ossl_stamp_file}"
            COMMAND "${CMAKE_COMMAND}" -E rm -f "${_ossl_building}"
        BUILD_BYPRODUCTS "${_ossl_lib}"
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_INSTALL ON
        LOG_OUTPUT_ON_FAILURE ON)
endif()

# --- the imported library ---------------------------------------------------
file(MAKE_DIRECTORY "${SYNBENCH_OPENSSL_ROOT}/include")
add_library(synbench_crypto STATIC IMPORTED GLOBAL)
set_target_properties(synbench_crypto PROPERTIES
    IMPORTED_LOCATION "${_ossl_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${SYNBENCH_OPENSSL_ROOT}/include")
add_dependencies(synbench_crypto synbench_openssl)
if(WIN32)
    set_property(TARGET synbench_crypto PROPERTY INTERFACE_LINK_LIBRARIES ws2_32 crypt32 advapi32 user32)
else()
    find_package(Threads REQUIRED)
    set_property(TARGET synbench_crypto PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads)
endif()
add_library(synbench::crypto ALIAS synbench_crypto)
set(SYNBENCH_OPENSSL_LIBRARY "${_ossl_lib}")
set(SYNBENCH_OPENSSL_BAKED_PREFIX "${_ossl_baked}")

# The licence text: LICENSE.txt of the verified 3.5.9 tarball, kept here because
# the source only arrives at build time and the notices are written at configure.
synbench_add_notice(NAME "OpenSSL (libcrypto)" VERSION ${SYNBENCH_OPENSSL_VERSION} LICENCE "Apache-2.0"
    FILE "${CMAKE_CURRENT_LIST_DIR}/openssl/LICENSE-OpenSSL-3.txt"
    NOTE "Only libcrypto is used, linked statically into synbench-cpucrypto.")
