# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 REQUIRED COMPONENTS Test)

# synbench_add_unit_test(<name> SOURCES ... LIBS ...)
# One Qt Test executable per test file, registered with CTest.
#
# Every test gets tests/support/crash_handler.cpp (a crash prints its cause
# instead of leaving an empty log) and writes verbose results to
# <build>/test-logs/<name>.txt as well as stdout, so CI can upload them.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-logs")

# No test may reach the user's real history or settings. In process, every
# test binary links tests/support/test_isolation.cpp (Qt test mode plus a
# guard that aborts unless the locations are under "qttest"). Around that,
# CTest runs every Qt test with scratch data/settings for any SynBench it
# launches (SYNBENCH_DATA_DIR, SYNBENCH_SETTINGS_FILE; dev-hook builds) and,
# on Unix, with HOME and the XDG data/config/cache/state homes in
# <build>/test-home and XDG_RUNTIME_DIR in a private 0700 folder; the
# test_home fixture recreates those first and fails afterwards if anything
# SynBench appeared outside Qt's test-mode root (tests/check_test_home.cmake).
set(SB_TEST_HOME "${CMAKE_BINARY_DIR}/test-home")
# The runtime folder (the single-instance sockets) is chosen when the tests
# run, not here: test_home_setup makes a random 0700 folder under the
# caller's own XDG_RUNTIME_DIR (or /tmp without one) and records it in
# <test-home>/runtime-dir; each test binary reads that file
# (SYNBENCH_TEST_RUNTIME_FILE) and sets XDG_RUNTIME_DIR from it, and
# test_home_untouched removes the folder again.
set(SB_TEST_ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen"
    "SYNBENCH_DATA_DIR=${SB_TEST_HOME}/scratch/data"
    "SYNBENCH_SETTINGS_FILE=${SB_TEST_HOME}/scratch/settings.ini")
set(SB_TEST_FIXTURES "")
if(UNIX)
    list(APPEND SB_TEST_ENVIRONMENT
        "HOME=${SB_TEST_HOME}"
        "XDG_DATA_HOME=${SB_TEST_HOME}/.local/share"
        "XDG_CONFIG_HOME=${SB_TEST_HOME}/.config"
        "XDG_CACHE_HOME=${SB_TEST_HOME}/.cache"
        "XDG_STATE_HOME=${SB_TEST_HOME}/.local/state"
        "SYNBENCH_TEST_RUNTIME_FILE=${SB_TEST_HOME}/runtime-dir")
    set(SB_TEST_FIXTURES test_home)
    add_test(NAME test_home_setup
        COMMAND ${CMAKE_COMMAND} "-DHOME_DIR=${SB_TEST_HOME}" -DMODE=setup -P "${CMAKE_CURRENT_SOURCE_DIR}/check_test_home.cmake")
    add_test(NAME test_home_untouched
        COMMAND ${CMAKE_COMMAND} "-DHOME_DIR=${SB_TEST_HOME}" -DMODE=check -P "${CMAKE_CURRENT_SOURCE_DIR}/check_test_home.cmake")
    set_tests_properties(test_home_setup PROPERTIES FIXTURES_SETUP test_home)
    set_tests_properties(test_home_untouched PROPERTIES FIXTURES_CLEANUP test_home)
endif()

# ...and both guards must themselves fail on a planted case: a binary that
# turns test mode off, and a test HOME holding a real-looking history folder.
qt_add_executable(planted_isolation "${CMAKE_CURRENT_SOURCE_DIR}/support/planted_isolation.cpp"
                                    "${CMAKE_CURRENT_SOURCE_DIR}/support/test_isolation.cpp")
target_link_libraries(planted_isolation PRIVATE Qt6::Core)
add_test(NAME test_isolation_rejects_planted COMMAND planted_isolation)
set_tests_properties(test_isolation_rejects_planted PROPERTIES
    PASS_REGULAR_EXPRESSION "is not a Qt test location" FAIL_REGULAR_EXPRESSION "planted test ran")
if(UNIX)
    set(_planted_home "${CMAKE_CURRENT_BINARY_DIR}/planted-home/test-home")
    file(MAKE_DIRECTORY "${_planted_home}/.local/share/SynBench" "${_planted_home}/.qttest/share/SynBench")
    add_test(NAME test_home_untouched_rejects_planted
        COMMAND ${CMAKE_COMMAND} "-DHOME_DIR=${_planted_home}" -DMODE=check -P "${CMAKE_CURRENT_SOURCE_DIR}/check_test_home.cmake")
    set_tests_properties(test_home_untouched_rejects_planted PROPERTIES
        PASS_REGULAR_EXPRESSION "test[ \n]+HOME:[ \n]+\\.local/share/SynBench\n"
        FAIL_REGULAR_EXPRESSION "\\.qttest/share/SynBench")
    # ...and the setup refuses an XDG_RUNTIME_DIR that is a planted symlink
    # or another user's (the sudo ctest case), before creating anything.
    set(_planted_runtime "${CMAKE_CURRENT_BINARY_DIR}/planted-runtime")
    file(MAKE_DIRECTORY "${_planted_runtime}/victim")
    file(CREATE_LINK "${_planted_runtime}/victim" "${_planted_runtime}/link" SYMBOLIC)
    add_test(NAME test_home_setup_rejects_symlinked_runtime
        COMMAND ${CMAKE_COMMAND} "-DHOME_DIR=${_planted_runtime}/test-home" -DMODE=setup
                -P "${CMAKE_CURRENT_SOURCE_DIR}/check_test_home.cmake")
    set_tests_properties(test_home_setup_rejects_symlinked_runtime PROPERTIES
        ENVIRONMENT "XDG_RUNTIME_DIR=${_planted_runtime}/link" PASS_REGULAR_EXPRESSION "is a symlink")
    execute_process(COMMAND id -u OUTPUT_VARIABLE _sb_uid OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _sb_uid STREQUAL "0")  # "/" is root's: someone else's, unless we are root
        add_test(NAME test_home_setup_rejects_foreign_runtime
            COMMAND ${CMAKE_COMMAND} "-DHOME_DIR=${_planted_runtime}/test-home" -DMODE=setup
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_test_home.cmake")
        set_tests_properties(test_home_setup_rejects_foreign_runtime PROPERTIES
            ENVIRONMENT "XDG_RUNTIME_DIR=/" PASS_REGULAR_EXPRESSION "isn't yours")
    endif()
endif()

function(synbench_isolate_test name)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${SB_TEST_ENVIRONMENT}" FIXTURES_REQUIRED "${SB_TEST_FIXTURES}")
endfunction()

function(synbench_add_unit_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    # test_isolation.cpp first (its initialiser is pinned to run first anyway).
    qt_add_executable(${name} "${PROJECT_SOURCE_DIR}/tests/support/test_isolation.cpp" ${ARG_SOURCES}
                              "${PROJECT_SOURCE_DIR}/tests/support/crash_handler.cpp")
    target_link_libraries(${name} PRIVATE Qt6::Test ${ARG_LIBS})
    add_test(NAME ${name}
        COMMAND ${name} -v2 -o "${CMAKE_BINARY_DIR}/test-logs/${name}.txt,txt" -o "-,txt")
    synbench_isolate_test(${name})
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

# tools/cpu-repeatability.py (§14.3) on small histories made with the app's
# own migrations, in a temporary folder (never the user's history).
if(SYNBENCH_PYTHON3)
    add_test(NAME cpu_repeatability_tool
        COMMAND "${SYNBENCH_PYTHON3}" -I "${PROJECT_SOURCE_DIR}/tests/tools/test_cpu_repeatability.py" "${PROJECT_SOURCE_DIR}")
endif()

# The GPU worker's committed SPIR-V and GLSL belong to the current shader
# sources (Phase 4a design §13.1): STAMP is the SHA-256 of the HLSL and the
# pinned tool versions (tools/gpu-shaders/build.py --check, no shader tools
# needed). And the check fails on a planted change: a copy of the shader
# folder with one space appended to present.hlsl.
if(SYNBENCH_PYTHON3)
    set(_sb_shaders "${PROJECT_SOURCE_DIR}/src/tests/gpu/worker/shaders")
    add_test(NAME gpu_shaders_stamp
        COMMAND "${SYNBENCH_PYTHON3}" -I "${PROJECT_SOURCE_DIR}/tools/gpu-shaders/build.py" --check)
    set(_sb_stale "${CMAKE_CURRENT_BINARY_DIR}/planted-shaders")
    file(REMOVE_RECURSE "${_sb_stale}")
    file(COPY "${_sb_shaders}/" DESTINATION "${_sb_stale}")
    file(APPEND "${_sb_stale}/present.hlsl" " ")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_sb_shaders}/present.hlsl" "${_sb_shaders}/generated/STAMP")
    add_test(NAME gpu_shaders_stamp_rejects_change
        COMMAND "${SYNBENCH_PYTHON3}" -I "${PROJECT_SOURCE_DIR}/tools/gpu-shaders/build.py"
                --check --expect-stale --sources "${_sb_stale}")
    set_tests_properties(gpu_shaders_stamp_rejects_change PROPERTIES PASS_REGULAR_EXPRESSION "out of date against its STAMP")
endif()

# THIRD-PARTY-NOTICES.txt has a section, with its licence text, for each
# component SynBench ships that isn't its own (D11): the pinned libraries,
# OpenSSL, Qt, the code bundled inside Qt (from Qt's SBOM, cmake/QtNotices.cmake),
# ICU on Linux, and the fonts. The list is kept here by hand, not derived from
# the deployed files.
set(_notices_expect
    "zlib ${SYNBENCH_ZLIB_VERSION}|This software is provided 'as-is'"
    "Zstandard (zstd) ${SYNBENCH_ZSTD_VERSION}|Redistribution and use in source and binary forms"
    "LZ4 (library) ${SYNBENCH_LZ4_VERSION}|Redistribution and use in source and binary forms"
    "XZ Utils (liblzma) ${SYNBENCH_XZ_VERSION}|Permission to use, copy, modify, and/or distribute"
    "volk ${SYNBENCH_VOLK_VERSION}|Permission is hereby granted, free of charge"
    "glad (OpenGL loader) ${SYNBENCH_GLAD_VERSION}|Creative Commons Legal Code"
    "Khronos OpenGL registry (in glad's loader)|Apache License"
    "Khronos platform header (KHR/khrplatform.h, in glad's loader)|Permission is hereby granted"
    "misc-fixed 5x7 font (X.Org font-misc-misc) 1.1.3|Public domain font."
    "OpenSSL (libcrypto) ${SYNBENCH_OPENSSL_VERSION}|Apache License"
    "Qt ${Qt6_VERSION}|GNU LESSER GENERAL PUBLIC LICENSE"
    "Third-party code inside Qt ${Qt6_VERSION}|Permission is hereby granted"
    "Archivo font|SIL OPEN FONT LICENSE Version 1.1"
    "IBM Plex Sans and IBM Plex Mono fonts|SIL OPEN FONT LICENSE Version 1.1")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _notices_expect "ICU ${SYNBENCH_QT_NOTICE_ICU}|UNICODE, INC. LICENSE AGREEMENT")
endif()
list(JOIN _notices_expect "@@" _notices_expect_arg)
add_test(NAME third_party_notices
    COMMAND "${CMAKE_COMMAND}" "-DNOTICES=${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt"
            "-DEXPECT=${_notices_expect_arg}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckThirdPartyNotices.cmake")
# The check must fail on a missing section and on a section without its
# licence text (AP-5 L2): planted entries that the file doesn't have.
add_test(NAME third_party_notices_rejects_missing_section
    COMMAND "${CMAKE_COMMAND}" "-DNOTICES=${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt"
            "-DEXPECT=zlib ${SYNBENCH_ZLIB_VERSION}|This software is provided 'as-is'@@Not a shipped component|anything"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckThirdPartyNotices.cmake")
add_test(NAME third_party_notices_rejects_missing_text
    COMMAND "${CMAKE_COMMAND}" "-DNOTICES=${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt"
            "-DEXPECT=zlib ${SYNBENCH_ZLIB_VERSION}|SIL OPEN FONT LICENSE Version 1.1"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckThirdPartyNotices.cmake")
set_tests_properties(third_party_notices_rejects_missing_section third_party_notices_rejects_missing_text
    PROPERTIES WILL_FAIL TRUE)

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
# The RAM test's kernel TUs: -O2 / /O2 in every build type, no LTO, no
# run-time checks (Phase 3 design §1, §9).
get_target_property(_rk_sources synbench_ram_kernels SOURCES)
get_target_property(_rk_dir synbench_ram_kernels SOURCE_DIR)
set(_rk_plain "")
foreach(_s IN LISTS _rk_sources)
    if(_s MATCHES "\\.cpp$")
        list(APPEND _rk_plain "${_rk_dir}/${_s}")
    endif()
endforeach()
# ...and the bandwidth paths (§5.3): the baseline ones the same, the wide
# ones with their arch flags too.
get_property(_bw_isas GLOBAL PROPERTY SYNBENCH_BW_ISAS)
set(_rk_wide "")
foreach(_isa IN LISTS _bw_isas)
    get_target_property(_flags synbench_bw_${_isa} SYNBENCH_ARCH_FLAGS)
    get_target_property(_dir synbench_bw_${_isa} SOURCE_DIR)
    if(_flags)
        list(JOIN _flags "," _joined)
        list(APPEND _rk_wide "${_dir}/bw/bw_${_isa}.cpp=${_joined}")
    else()
        list(APPEND _rk_plain "${_dir}/bw/bw_${_isa}.cpp")
    endif()
endforeach()
list(JOIN _rk_plain "|" _rk_plain)
list(JOIN _rk_wide "|" _rk_wide)
add_test(NAME ram_kernel_flags
    COMMAND "${CMAKE_COMMAND}" "-DCOMMANDS=${PROJECT_BINARY_DIR}/compile_commands.json" -DCOMPILER=${_k_compiler}
            "-DPLAIN=${_rk_plain}" "-DWIDE=${_rk_wide}" -P "${PROJECT_SOURCE_DIR}/cmake/CheckKernelFlags.cmake")
# ...and SB_NO_VECTOR (MSVC's loop pragma) is always followed by a classic
# loop, anywhere under src/; planted range-fors (on the next line, on the
# same line, with an init-statement) must each fail.
add_test(NAME cpu_no_vector_pragmas
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}" -P "${PROJECT_SOURCE_DIR}/cmake/CheckNoVectorPragmas.cmake")
set_tests_properties(cpu_no_vector_pragmas PROPERTIES FAIL_REGULAR_EXPRESSION " 0 SB_NO_VECTOR site")
foreach(_planted next_line same_line init_statement)
    if(_planted STREQUAL "next_line")
        set(_body "    SB_NO_VECTOR\n    for (float &x : c)\n        x = 0;\n")
    elseif(_planted STREQUAL "same_line")
        set(_body "    SB_NO_VECTOR for (float &x : c)\n        x = 0;\n")
    else()
        set(_body "    SB_NO_VECTOR\n    for (int i = 0; float &x : c)\n        x = float(i++);\n")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/planted-flags/range_for_${_planted}.cpp"
         "void f(float (&c)[4])\n{\n${_body}}\n")
    add_test(NAME cpu_no_vector_pragmas_rejects_${_planted}
        COMMAND "${CMAKE_COMMAND}" "-DFILES=${CMAKE_CURRENT_BINARY_DIR}/planted-flags/range_for_${_planted}.cpp"
                -P "${PROJECT_SOURCE_DIR}/cmake/CheckNoVectorPragmas.cmake")
    set_tests_properties(cpu_no_vector_pragmas_rejects_${_planted} PROPERTIES
        PASS_REGULAR_EXPRESSION "range_for_${_planted}\\.cpp: SB_NO_VECTOR is followed by")
endforeach()
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
    # The RAM test's bandwidth paths too (Phase 3 design §5.3, P8).
    foreach(_isa IN LISTS _bw_isas)
        list(APPEND _simd_objects "bw:${_isa}=$<TARGET_OBJECTS:synbench_bw_${_isa}>")
    endforeach()
    add_test(NAME simd_odr
        COMMAND "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/simd-odr.py" --nm "${SYNBENCH_NM}"
                ${_simd_objects})
    add_subdirectory(simd_planted)
    add_test(NAME simd_odr_rejects_planted
        COMMAND "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/simd-odr.py" --nm "${SYNBENCH_NM}"
                --expect-violation "planted=$<TARGET_OBJECTS:synbench_simd_planted>")
    set_tests_properties(simd_odr_rejects_planted PROPERTIES PASS_REGULAR_EXPRESSION "plantedOutside")
    add_test(NAME simd_odr_rejects_planted_initialiser
        COMMAND "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/simd-odr.py" --nm "${SYNBENCH_NM}"
                --expect-violation "planted=$<TARGET_OBJECTS:synbench_simd_planted_init>")
    set_tests_properties(simd_odr_rejects_planted_initialiser PROPERTIES PASS_REGULAR_EXPRESSION "dynamic initialiser")
    # ...and under the bandwidth paths' rule (the same planted object).
    add_test(NAME simd_odr_rejects_planted_bw
        COMMAND "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/simd-odr.py" --nm "${SYNBENCH_NM}"
                --expect-violation "bw:planted=$<TARGET_OBJECTS:synbench_simd_planted>")
    set_tests_properties(simd_odr_rejects_planted_bw PROPERTIES PASS_REGULAR_EXPRESSION "outside sb::ram::bw_planted.*\n.*plantedOutside")
    list(JOIN _simd_wide_flags "|" _simd_wide_flags)
    add_test(NAME cpu_simd_flags
        COMMAND "${CMAKE_COMMAND}" "-DCOMMANDS=${PROJECT_BINARY_DIR}/compile_commands.json" -DCOMPILER=${_k_compiler}
                "-DWIDE=${_simd_wide_flags}" -P "${PROJECT_SOURCE_DIR}/cmake/CheckKernelFlags.cmake")
endif()

# The RAM test's x64 store variants (Phase 3 design D4, §14.1): the scored
# write/copy use non-temporal stores and SFENCE, the plain ones don't, and no
# entry point calls out (packaging/checks/bw-stores.py, on the objects'
# code); and the check fails on a planted object that breaks each rule.
if(WIN32)
    set(SYNBENCH_DISASM "${SYNBENCH_OBJDUMP}")
else()
    find_program(SYNBENCH_DISASM NAMES objdump llvm-objdump)
endif()
set(_bw_x64 "")
foreach(_isa IN LISTS _bw_isas)
    if(_isa MATCHES "^(sse2|avx2)$")
        list(APPEND _bw_x64 "${_isa}=$<TARGET_OBJECTS:synbench_bw_${_isa}>")
    endif()
endforeach()
if(SYNBENCH_PYTHON3 AND SYNBENCH_DISASM AND _bw_x64)
    set(_bw_check "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/bw-stores.py" --objdump "${SYNBENCH_DISASM}")
    add_test(NAME bw_stores COMMAND ${_bw_check} ${_bw_x64})
    add_subdirectory(bw_planted)
    foreach(_rule nt plain call)
        if(_rule STREQUAL "nt")
            set(_expect "sb_bw_planted_write_nt: no non-temporal store")
        elseif(_rule STREQUAL "plain")
            set(_expect "sb_bw_planted_write: non-temporal store")
        else()
            set(_expect "sb_bw_planted_copy: (calls out|branches to another symbol)")
        endif()
        add_test(NAME bw_stores_rejects_planted_${_rule}
            COMMAND ${_bw_check} --expect-violation "planted=$<TARGET_OBJECTS:synbench_bw_planted>")
        set_tests_properties(bw_stores_rejects_planted_${_rule} PROPERTIES PASS_REGULAR_EXPRESSION "${_expect}")
    endforeach()
endif()

# The encryption worker's golden table (§5.5) is what the worker computes on
# this platform (it is made by the worker itself: OpenSSL is linked into the
# worker only).
add_test(NAME cpu_crypto_golden_table
    COMMAND "${CMAKE_COMMAND}" "-DCOMMAND=$<TARGET_FILE:synbench-cpucrypto>" -DARGS=--print-golden
            "-DEXPECTED=${PROJECT_SOURCE_DIR}/src/tests/cpu/golden/crypto.inc"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckCommandOutput.cmake")

# The encryption worker's imports load from System32 and its folder only
# (/DEPENDENTLOADFLAG:0xA00, AP-5 L3), read from its PE header; and the check
# fails on a planted executable linked without the flag (MSVC builds).
if(MSVC AND SYNBENCH_PYTHON3)
    add_test(NAME pe_loadflags_worker
        COMMAND "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/pe-loadflags.py"
                "$<TARGET_FILE:synbench-cpucrypto>" --expect 0xA00)
    add_subdirectory(pe_planted)
    add_test(NAME pe_loadflags_rejects_planted
        COMMAND "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/pe-loadflags.py"
                "$<TARGET_FILE:pe_planted_noflag>" --expect 0xA00)
    set_tests_properties(pe_loadflags_rejects_planted PROPERTIES PASS_REGULAR_EXPRESSION "FAIL .*(DependentLoadFlags 0x0,|no load configuration)")
endif()

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
# ...and the length limit fails on a planted 201-character path (the Tiamat
# capture's sysfs device tree broke the Windows checkout that way), but not
# on one of exactly 200.
string(REPEAT "a" 194 _sb_long)
add_test(NAME windows_safe_paths_rejects_long
    COMMAND "${CMAKE_COMMAND}" "-DFILES=ok/file;tests/${_sb_long}x"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckWindowsPaths.cmake")
set_tests_properties(windows_safe_paths_rejects_long PROPERTIES
    PASS_REGULAR_EXPRESSION "201: tests/aaaa" FAIL_REGULAR_EXPRESSION "ok/file")
add_test(NAME windows_safe_paths_accepts_200
    COMMAND "${CMAKE_COMMAND}" "-DFILES=tests/${_sb_long}" -P "${PROJECT_SOURCE_DIR}/cmake/CheckWindowsPaths.cmake")
set_tests_properties(windows_safe_paths_accepts_200 PROPERTIES PASS_REGULAR_EXPRESSION "the longest is 200 characters")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(helper)
endif()
