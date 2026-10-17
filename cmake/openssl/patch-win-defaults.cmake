# SPDX-License-Identifier: GPL-3.0-or-later
# OpenSSL 3.5.9's get_windows_regdirs() (crypto/defaults.c, built with
# OSSL_WINCTX, which SynBench's Windows builds use) declares `HKEY hkey;`
# without a value; when RegOpenKeyEx fails (the normal case for SynBench: the
# registry key doesn't exist) it still calls RegCloseKey(hkey) on that stack
# garbage. This initialises it to NULL (RegCloseKey(NULL) only returns an
# error). To be offered upstream with the armcap patch (AP-5 L9).
#
# Not changed, reported instead: the same RegOpenKeyEx passes KEY_WOW64_32KEY
# as ulOptions instead of in samDesired. Moving it isn't clearly right (the
# key path already names WOW6432Node explicitly), and for SynBench the key is
# never meant to be read: the directories stay NULL either way.
#
# Applied by ExternalProject's PATCH_COMMAND on every target (Windows-only
# code; every job proves it still applies); fails loudly unless the original
# line is there exactly once (design R2). The marker carries this script's
# hash, like patch-win-armcap.cmake.
#   cmake -DSOURCE_DIR=<openssl source> -P patch-win-defaults.cmake
# Run with cmake -P: the policies of the project's minimum.
cmake_minimum_required(VERSION 3.25)
set(file "${SOURCE_DIR}/crypto/defaults.c")
file(READ "${file}" text)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" script_sha)
string(SUBSTRING "${script_sha}" 0 16 script_sha)
set(marker "SynBench (AP-5 L9, patch ${script_sha})")
string(FIND "${text}" "${marker}" already)
if(NOT already EQUAL -1)
    message(STATUS "defaults patch: already applied")
    return()
endif()
string(FIND "${text}" "SynBench (AP-5 L9" other)
if(NOT other EQUAL -1)
    message(FATAL_ERROR "defaults patch: ${file} was patched by another version of this script; "
                        "delete the OpenSSL source tree so it is extracted again")
endif()
set(before "    DWORD ktype;\n    HKEY hkey;\n    LSTATUS ret;\n")
set(after "    DWORD ktype;\n    HKEY hkey = NULL; /* @MARKER@: closed below even when not opened */\n    LSTATUS ret;\n")
string(FIND "${text}" "${before}" first)
string(FIND "${text}" "${before}" last REVERSE)
if(first EQUAL -1 OR NOT first EQUAL last)
    message(FATAL_ERROR "defaults patch: get_windows_regdirs in ${file} isn't the expected 3.5.9 code; "
                        "update cmake/openssl/patch-win-defaults.cmake for this OpenSSL")
endif()
string(REPLACE "@MARKER@" "${marker}" after "${after}")
string(REPLACE "${before}" "${after}" text "${text}")
file(WRITE "${file}" "${text}")
message(STATUS "defaults patch: applied to ${file}")
