# SPDX-License-Identifier: GPL-3.0-or-later
# OpenSSL's Windows ARM64 cpuid setup ignores OPENSSL_armcap (Phase 2 design
# N5, D5: the same in 3.5.9, 4.0.3 and master), so Syntheon couldn't force
# the software path on a Snapdragon. This adds the override the other
# platforms already have (crypto/armcap.c's non-Windows OPENSSL_cpuid_setup:
# the value replaces the detected capabilities). To be offered upstream.
#
# Applied by ExternalProject's PATCH_COMMAND on every target (the code is
# Windows-only, so elsewhere it changes nothing, but every job proves it still
# applies). It fails loudly if the original text isn't there exactly once, so
# an OpenSSL bump can't silently drop it (design R2).
#   cmake -DSOURCE_DIR=<openssl source> -P patch-win-armcap.cmake
# Run with cmake -P: the policies of the project's minimum (a script gets
# none from the project, and CMake 3.x would run it with OLD ones).
cmake_minimum_required(VERSION 3.25)
set(file "${SOURCE_DIR}/crypto/armcap.c")
file(READ "${file}" text)
# The marker names this script's own hash (L3): a tree patched by another
# version of the script is an error, not "already applied".
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" script_sha)
string(SUBSTRING "${script_sha}" 0 16 script_sha)
set(marker "Syntheon (Phase 2 D5, patch ${script_sha})")
string(FIND "${text}" "${marker}" already)
if(NOT already EQUAL -1)
    message(STATUS "armcap patch: already applied")
    return()
endif()
string(FIND "${text}" "Syntheon (Phase 2 D5" other)
if(NOT other EQUAL -1)
    message(FATAL_ERROR "armcap patch: ${file} was patched by another version of this script; "
                        "delete the OpenSSL source tree so it is extracted again")
endif()
set(before [[
#ifdef _WIN32
void OPENSSL_cpuid_setup(void)
{
    OPENSSL_armcap_P |= ARMV7_NEON;
]])
set(after [[
#ifdef _WIN32
void OPENSSL_cpuid_setup(void)
{
    const char *e;

    /*
     * @MARKER@: honour OPENSSL_armcap as the other platforms
     * do, so a run can force the software path. The value replaces the
     * detected capabilities.
     */
    if ((e = getenv("OPENSSL_armcap")) != NULL) {
        OPENSSL_armcap_P = (unsigned int)strtoul(e, NULL, 0);
        return;
    }
    OPENSSL_armcap_P |= ARMV7_NEON;
]])
string(FIND "${text}" "${before}" first)
string(FIND "${text}" "${before}" last REVERSE)
if(first EQUAL -1 OR NOT first EQUAL last)
    message(FATAL_ERROR "armcap patch: the Windows OPENSSL_cpuid_setup in ${file} isn't the expected 3.5.9 code; "
                        "update cmake/openssl/patch-win-armcap.cmake for this OpenSSL")
endif()
string(REPLACE "@MARKER@" "${marker}" after "${after}")
string(REPLACE "${before}" "${after}" text "${text}")
file(WRITE "${file}" "${text}")
message(STATUS "armcap patch: applied to ${file}")
