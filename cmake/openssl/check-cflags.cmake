# SPDX-License-Identifier: GPL-3.0-or-later
# After OpenSSL's Configure: its generated makefile must still compile
# optimised (an environment CFLAGS once replaced OpenSSL's own -O3 and gave
# an -O0 libcrypto). Fails unless the CFLAGS line has -O3 (make) or /O2
# (nmake), and copies that line to OUT, which the install step puts into the
# OpenSSL root (syntheon-openssl-cflags.txt; the openssl_config CTest reads it),
# with the CPPFLAGS line.
#   cmake -DMAKEFILE=<build>/Makefile|makefile -DWANT=-O3|/O2 -DOUT=<file> -P check-cflags.cmake
# Run with cmake -P: the policies of the project's minimum (a script gets
# none from the project, and CMake 3.x would run it with OLD ones).
cmake_minimum_required(VERSION 3.25)
file(STRINGS "${MAKEFILE}" lines REGEX "^CFLAGS=")
list(LENGTH lines n)
if(n EQUAL 0)
    message(FATAL_ERROR "No CFLAGS= line in ${MAKEFILE}")
endif()
list(GET lines 0 cflags)
string(REPLACE " " ";" flags "${cflags}")
string(REPLACE "CFLAGS=" "" flags "${flags}")
if(NOT WANT IN_LIST flags)
    message(FATAL_ERROR "OpenSSL would build unoptimised: ${MAKEFILE} has '${cflags}', without ${WANT}")
endif()
# Also recorded, as evidence for the openssl_config CTest: the CPPFLAGS and
# LIB_CPPFLAGS lines (the user defines, -DOSSL_WINCTX on Windows, are there)
# and the rule that compiles defaults.c (it uses $(LIB_CFLAGS) $(LIB_CPPFLAGS)).
file(STRINGS "${MAKEFILE}" cpp REGEX "^(CPPFLAGS|LIB_CPPFLAGS)=")
file(STRINGS "${MAKEFILE}" all)
set(rule "")
set(take 0)
foreach(line IN LISTS all)
    if(take GREATER 0)
        string(APPEND rule "${line}\n")
        math(EXPR take "${take} - 1")
    elseif(line MATCHES "libcrypto-lib-defaults\\.o(bj)?:")
        string(APPEND rule "${line}\n")
        set(take 1)
    endif()
endforeach()
list(JOIN cpp "\n" cpp)
file(WRITE "${OUT}" "${cflags}\n${cpp}\n${rule}")
message(STATUS "OpenSSL ${cflags}")
