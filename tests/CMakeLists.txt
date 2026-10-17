# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 REQUIRED COMPONENTS Test)

# synbench_add_unit_test(<name> SOURCES ... LIBS ...)
# One Qt Test executable per test file, registered with CTest.
#
# Every test gets tests/support/crash_handler.cpp (a crash prints its cause
# instead of leaving an empty log) and writes verbose results to
# <build>/test-logs/<name>.txt as well as stdout, so CI can upload them.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-logs")
function(synbench_add_unit_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    qt_add_executable(${name} ${ARG_SOURCES} "${PROJECT_SOURCE_DIR}/tests/support/crash_handler.cpp")
    target_link_libraries(${name} PRIVATE Qt6::Test ${ARG_LIBS})
    add_test(NAME ${name}
        COMMAND ${name} -v2 -o "${CMAKE_BINARY_DIR}/test-logs/${name}.txt,txt" -o "-,txt")
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

add_subdirectory(unit)
add_subdirectory(qml)

# The shipping-tree check must itself fail on a planted test binary (§10).
if(UNIX)
    add_test(NAME no_test_binaries_selftest
        COMMAND sh -c "d=$(mktemp -d) && mkdir -p \"$d/usr/bin\" && touch \"$d/usr/bin/SynBench\" && bash \"$0\" \"$d\" && touch \"$d/usr/bin/synbench-helper-test\" && ! bash \"$0\" \"$d\" 2>/dev/null; r=$?; rm -rf \"$d\"; exit $r"
                "${PROJECT_SOURCE_DIR}/packaging/checks/no-test-binaries.sh")
endif()

# The qmllint gate (CI's "any QML warning fails") must itself fail on a planted
# warning, with the real qmllint of this Qt.
find_program(SYNBENCH_PYTHON3 python3)
if(UNIX AND SYNBENCH_PYTHON3 AND TARGET Qt6::qmllint)
    add_test(NAME qmllint_gate_selftest
        COMMAND bash "${PROJECT_SOURCE_DIR}/packaging/checks/test-qmllint-gate.sh"
                "$<TARGET_FILE:Qt6::qmllint>" "${PROJECT_SOURCE_DIR}/packaging/checks/qmllint-gate.py")
endif()

# THIRD-PARTY-NOTICES.txt covers everything shipped (D11), each with its text.
add_test(NAME third_party_notices
    COMMAND "${CMAKE_COMMAND}" "-DNOTICES=${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt"
            "-DEXPECT=zlib ${SYNBENCH_ZLIB_VERSION}|This software is provided 'as-is';Zstandard (zstd) ${SYNBENCH_ZSTD_VERSION}|Redistribution and use in source and binary forms;LZ4 (library) ${SYNBENCH_LZ4_VERSION}|Redistribution and use in source and binary forms;XZ Utils (liblzma) ${SYNBENCH_XZ_VERSION}|Permission to use, copy, modify, and/or distribute;OpenSSL (libcrypto) ${SYNBENCH_OPENSSL_VERSION}|Apache License;Qt ${Qt6_VERSION}|GNU LESSER GENERAL PUBLIC LICENSE;Archivo font|SIL OPEN FONT LICENSE Version 1.1;IBM Plex Sans and IBM Plex Mono fonts|SIL OPEN FONT LICENSE Version 1.1"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckThirdPartyNotices.cmake")

# The built libcrypto has the hardware AES and SHA-256 code (§11.2), and the
# check fails without it: on a library that isn't libcrypto, and (self-test)
# with the objects that provide each part removed.
if(UNIX AND SYNBENCH_PYTHON3 AND NOT CMAKE_CROSSCOMPILING)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sb_cpu)
    if(_sb_cpu MATCHES "^(x86_64|amd64)$")
        set(_sb_crypto_arch x86_64)
    else()
        set(_sb_crypto_arch aarch64)
    endif()
    add_test(NAME crypto_symbols
        COMMAND "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/crypto-symbols.py"
                "${SYNBENCH_OPENSSL_LIBRARY}" --arch ${_sb_crypto_arch} --self-test)
    add_test(NAME crypto_symbols_rejects_other_library
        COMMAND "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/crypto-symbols.py"
                "$<TARGET_FILE:zlibstatic>" --arch ${_sb_crypto_arch})
    set_tests_properties(crypto_symbols_rejects_other_library PROPERTIES WILL_FAIL TRUE)
endif()

# OpenSSL was configured with P17's options and kept its assembly.
add_test(NAME openssl_config
    COMMAND "${CMAKE_COMMAND}" "-DCONFIG_H=${SYNBENCH_OPENSSL_ROOT}/include/openssl/configuration.h"
            -P "${PROJECT_SOURCE_DIR}/cmake/openssl/check-config.cmake")

# Every tracked path must be checkout-able on Windows.
add_test(NAME windows_safe_paths
    COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/CheckWindowsPaths.cmake")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(helper)
endif()
