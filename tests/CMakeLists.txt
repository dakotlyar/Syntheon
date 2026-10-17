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
find_program(SYNBENCH_PYTHON3 NAMES python3 python)
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
# check fails without it (AP-5 M1, M2): on a library that isn't libcrypto
# (both parts missing, and readable: exit 1, not 2) and on planted archives
# with one part a stub or plain C (x86-64, GCC/Clang).
if(WIN32)
    find_program(SYNBENCH_NM llvm-nm HINTS "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin" "$ENV{ProgramFiles}/LLVM/bin")
    # COFF has no symbol sizes: sections and their sizes come from llvm-objdump.
    find_program(SYNBENCH_OBJDUMP llvm-objdump HINTS "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin" "$ENV{ProgramFiles}/LLVM/bin")
else()
    find_program(SYNBENCH_NM nm)
endif()
if(SYNBENCH_PYTHON3 AND SYNBENCH_NM AND NOT CMAKE_CROSSCOMPILING)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sb_cpu)
    if(_sb_cpu MATCHES "^(x86_64|amd64)$")
        set(_sb_crypto_arch x86_64)
    else()
        set(_sb_crypto_arch aarch64)
    endif()
    set(_sb_crypto_check "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/crypto-symbols.py" --nm "${SYNBENCH_NM}"
        --arch ${_sb_crypto_arch})
    if(SYNBENCH_OBJDUMP)
        list(APPEND _sb_crypto_check --objdump "${SYNBENCH_OBJDUMP}")
    endif()
    add_test(NAME crypto_symbols COMMAND ${_sb_crypto_check} "${SYNBENCH_OPENSSL_LIBRARY}")
    add_test(NAME crypto_symbols_rejects_other_library
        COMMAND ${_sb_crypto_check} "$<TARGET_FILE:zlibstatic>" --expect-missing all)
    if(_sb_crypto_arch STREQUAL "x86_64" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        add_subdirectory(crypto_planted)
        add_test(NAME crypto_symbols_rejects_stub_gcm
            COMMAND ${_sb_crypto_check} "$<TARGET_FILE:planted_crypto_stub_aes>" --expect-missing AES)
        add_test(NAME crypto_symbols_rejects_c_sha256
            COMMAND ${_sb_crypto_check} "$<TARGET_FILE:planted_crypto_c_sha>" --expect-missing SHA-256)
    endif()
endif()
# The COFF (Windows) path of the check, on planted archives (AP-5 L8): where
# clang and the LLVM tools are around (next to clang, or versioned on PATH).
if(NOT WIN32 AND SYNBENCH_PYTHON3)
    find_program(SYNBENCH_CLANG NAMES clang clang-18 clang-17 clang-16 clang-15 clang-14)
    set(_llvm_hints "")
    if(SYNBENCH_CLANG)
        get_filename_component(_clang_real "${SYNBENCH_CLANG}" REALPATH)
        get_filename_component(_llvm_bin "${_clang_real}" DIRECTORY)
        set(_llvm_hints HINTS "${_llvm_bin}")
    endif()
    find_program(SYNBENCH_LLVM_AR NAMES llvm-ar ${_llvm_hints})
    find_program(SYNBENCH_LLVM_NM NAMES llvm-nm ${_llvm_hints})
    find_program(SYNBENCH_LLVM_OBJDUMP NAMES llvm-objdump ${_llvm_hints})
    if(SYNBENCH_CLANG AND SYNBENCH_LLVM_AR AND SYNBENCH_LLVM_NM AND SYNBENCH_LLVM_OBJDUMP)
        add_subdirectory(crypto_planted/coff)
    else()
        message(STATUS "No clang + LLVM tools: the COFF planted crypto-symbols tests are skipped")
    endif()
endif()

# OpenSSL was configured with P17's options, kept its assembly, was built
# optimised (H2) and bakes in only the dummy directories (M4).
add_test(NAME openssl_config
    COMMAND "${CMAKE_COMMAND}" "-DROOT=${SYNBENCH_OPENSSL_ROOT}" "-DLIBRARY=${SYNBENCH_OPENSSL_LIBRARY}"
            "-DBAKED=${SYNBENCH_OPENSSL_BAKED_PREFIX}" "-DWINCTX=${SYNBENCH_OPENSSL_WINCTX}"
            -P "${PROJECT_SOURCE_DIR}/cmake/openssl/check-config.cmake")

# The CPU test's golden tables are what the reference implementations give on
# this platform (§6.4): the same committed table must hold everywhere.
add_test(NAME cpu_golden_tables
    COMMAND synbench-cpu-golden --check "${PROJECT_SOURCE_DIR}/src/tests/cpu/golden")

# The kernel TUs get P7's flags (§6.2, §14.2 item 4), as compile_commands.json
# records them; and the check fails on a planted TU built at -O3 without them.
get_target_property(_k_sources synbench_cpu_kernels SOURCES)
get_target_property(_k_dir synbench_cpu_kernels SOURCE_DIR)
set(_k_scalar "")
foreach(_s IN LISTS _k_sources)
    if(_s MATCHES "\\.cpp$")
        list(APPEND _k_scalar "${_k_dir}/${_s}")
    endif()
endforeach()
list(JOIN _k_scalar "|" _k_scalar)
if(MSVC)
    set(_k_compiler MSVC)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_k_compiler Clang)
else()
    set(_k_compiler GNU)
endif()
add_test(NAME cpu_kernel_flags
    COMMAND "${CMAKE_COMMAND}" "-DCOMMANDS=${PROJECT_BINARY_DIR}/compile_commands.json" -DCOMPILER=${_k_compiler}
            "-DSCALAR=${_k_scalar}" -P "${PROJECT_SOURCE_DIR}/cmake/CheckKernelFlags.cmake")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/planted-flags/compile_commands.json"
     "[{\"directory\": \"/b\", \"file\": \"/s/planted.cpp\", \"command\": \"c++ -O2 -fno-tree-vectorize -ffp-contract=off -O3 -c /s/planted.cpp\"}]\n")
add_test(NAME cpu_kernel_flags_rejects_planted
    COMMAND "${CMAKE_COMMAND}" "-DCOMMANDS=${CMAKE_CURRENT_BINARY_DIR}/planted-flags/compile_commands.json" -DCOMPILER=GNU
            -DSCALAR=/s/planted.cpp -P "${PROJECT_SOURCE_DIR}/cmake/CheckKernelFlags.cmake")
set_tests_properties(cpu_kernel_flags_rejects_planted PROPERTIES
    PASS_REGULAR_EXPRESSION "planted\\.cpp: optimisation is '-O3', not -O2")

# The SIMD paths' wide-arch objects follow the ODR rule (§5.3, P8): only
# their entry point and their own namespace (packaging/checks/simd-odr.py);
# and the check fails on a planted object that breaks it. The arch flags of
# each path's TU are checked too (compile_commands.json).
get_property(_simd_isas GLOBAL PROPERTY SYNBENCH_SIMD_ISAS)
if(SYNBENCH_PYTHON3 AND SYNBENCH_NM AND _simd_isas)
    set(_simd_objects "")
    set(_simd_wide_flags "")
    set(SYNBENCH_PLANTED_FLAGS "")
    foreach(_isa IN LISTS _simd_isas)
        list(APPEND _simd_objects "${_isa}=$<TARGET_OBJECTS:synbench_simd_${_isa}>")
        get_target_property(_flags synbench_simd_${_isa} SYNBENCH_ARCH_FLAGS)
        get_target_property(_wide synbench_simd_${_isa} SYNBENCH_WIDE)
        get_target_property(_dir synbench_simd_${_isa} SOURCE_DIR)
        set(_entry "${_dir}/simd/simd_${_isa}.cpp=")
        if(_flags)
            list(JOIN _flags "," _joined)
            string(APPEND _entry "${_joined}")
        endif()
        list(APPEND _simd_wide_flags "${_entry}")
        if(_wide AND NOT SYNBENCH_PLANTED_FLAGS)
            set(SYNBENCH_PLANTED_FLAGS ${_flags})
        endif()
    endforeach()
    add_test(NAME simd_odr
        COMMAND "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/simd-odr.py" --nm "${SYNBENCH_NM}"
                ${_simd_objects})
    add_subdirectory(simd_planted)
    add_test(NAME simd_odr_rejects_planted
        COMMAND "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/simd-odr.py" --nm "${SYNBENCH_NM}"
                --expect-violation "planted=$<TARGET_OBJECTS:synbench_simd_planted>")
    set_tests_properties(simd_odr_rejects_planted PROPERTIES PASS_REGULAR_EXPRESSION "plantedOutside")
    list(JOIN _simd_wide_flags "|" _simd_wide_flags)
    add_test(NAME cpu_simd_flags
        COMMAND "${CMAKE_COMMAND}" "-DCOMMANDS=${PROJECT_BINARY_DIR}/compile_commands.json" -DCOMPILER=${_k_compiler}
                "-DWIDE=${_simd_wide_flags}" -P "${PROJECT_SOURCE_DIR}/cmake/CheckKernelFlags.cmake")
endif()

# The encryption worker's golden table (§5.5) is what the worker computes on
# this platform (it is made by the worker itself: OpenSSL is linked into the
# worker only).
add_test(NAME cpu_crypto_golden_table
    COMMAND "${CMAKE_COMMAND}" "-DCOMMAND=$<TARGET_FILE:synbench-cpucrypto>" -DARGS=--print-golden
            "-DEXPECTED=${PROJECT_SOURCE_DIR}/src/tests/cpu/golden/crypto.inc"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckCommandOutput.cmake")

# Every script the build runs with `cmake -P` pins its policies.
add_test(NAME cmake_scripts_set_policies
    COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/CheckScriptPolicies.cmake")
# ...and fails on a planted -P script without the line.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/planted-policy/planted.cmake"
     "add_custom_target(x COMMAND \"\${CMAKE_COMMAND}\" -P \"\${CMAKE_CURRENT_LIST_DIR}/no-minimum.cmake\")\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/planted-policy/no-minimum.cmake" "message(STATUS \"no policies\")\n")
add_test(NAME cmake_scripts_set_policies_rejects_planted
    COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            "-DPLANTED=${CMAKE_CURRENT_BINARY_DIR}/planted-policy/planted.cmake"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckScriptPolicies.cmake")
set_tests_properties(cmake_scripts_set_policies_rejects_planted PROPERTIES
    PASS_REGULAR_EXPRESSION "planted-policy/no-minimum\\.cmake")

# Every tracked path must be checkout-able on Windows.
add_test(NAME windows_safe_paths
    COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/CheckWindowsPaths.cmake")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(helper)
endif()
