# SPDX-License-Identifier: GPL-3.0-or-later
# A command's standard output must equal a committed file, byte for byte
# (the encryption worker's golden table: synbench-cpucrypto --print-golden
# against src/tests/cpu/golden/crypto.inc).
#   cmake -DCOMMAND=<program> [-DARGS=<arg>|<arg>...] -DEXPECTED=<file> -P CheckCommandOutput.cmake
# Run with cmake -P: the policies of the project's minimum (a script gets
# none from the project, and CMake 3.x would run it with OLD ones).
cmake_minimum_required(VERSION 3.25)
string(REPLACE "|" ";" args "${ARGS}")
execute_process(COMMAND "${COMMAND}" ${args} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${COMMAND} ${ARGS} failed (${rc})")
endif()
file(READ "${EXPECTED}" want)
# Windows: the program's text-mode stdout writes CRLF.
string(REPLACE "\r\n" "\n" out "${out}")
string(REPLACE "\r\n" "\n" want "${want}")
if(NOT out STREQUAL want)
    message(FATAL_ERROR "${COMMAND} ${ARGS} differs from ${EXPECTED}; it gives:\n${out}")
endif()
message(STATUS "ok  ${EXPECTED}")
