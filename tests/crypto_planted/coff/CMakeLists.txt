# SPDX-License-Identifier: GPL-3.0-or-later
# COFF planted archives for crypto-symbols.py (AP-5 L8), made on any host with
# clang + llvm-ar: COFF has no symbol sizes, so this is the path that measures
# to the next function/global in the section or to the section's end.
#   stub.lib  AES-NI GCM stubs (3 bytes each) + a real-sized SHA-NI module
#   real.lib  real-sized AES-NI GCM (the encrypt entry last in .text, a global
#             in .rdata after it) + the same SHA-NI module
# Object names matter: the check looks for the sha256-x86_64 object.
set(_coff_flags --target=x86_64-pc-windows-msvc -c -x assembler)
set(_dir "${CMAKE_CURRENT_BINARY_DIR}")
foreach(kind stub real)
    file(MAKE_DIRECTORY "${_dir}/${kind}")
    add_custom_command(
        OUTPUT "${_dir}/${kind}.lib"
        COMMAND "${SYNBENCH_CLANG}" ${_coff_flags} "${CMAKE_CURRENT_SOURCE_DIR}/${kind}-aesni-gcm-x86_64.s"
                -o "${_dir}/${kind}/aesni-gcm-x86_64.obj"
        COMMAND "${SYNBENCH_CLANG}" ${_coff_flags} "${CMAKE_CURRENT_SOURCE_DIR}/sha256-x86_64.s"
                -o "${_dir}/${kind}/sha256-x86_64.obj"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${_dir}/${kind}.lib"
        COMMAND "${SYNBENCH_LLVM_AR}" rcs "${_dir}/${kind}.lib" aesni-gcm-x86_64.obj sha256-x86_64.obj
        WORKING_DIRECTORY "${_dir}/${kind}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${kind}-aesni-gcm-x86_64.s" "${CMAKE_CURRENT_SOURCE_DIR}/sha256-x86_64.s"
        VERBATIM)
endforeach()
add_custom_target(planted_crypto_coff ALL DEPENDS "${_dir}/stub.lib" "${_dir}/real.lib")
set(_check "${SYNBENCH_PYTHON3}" "${PROJECT_SOURCE_DIR}/packaging/checks/crypto-symbols.py"
    --arch x86_64 --nm "${SYNBENCH_LLVM_NM}" --objdump "${SYNBENCH_LLVM_OBJDUMP}")
add_test(NAME crypto_symbols_coff_rejects_stub_gcm COMMAND ${_check} "${_dir}/stub.lib" --expect-missing AES)
add_test(NAME crypto_symbols_coff_accepts_real COMMAND ${_check} "${_dir}/real.lib")
