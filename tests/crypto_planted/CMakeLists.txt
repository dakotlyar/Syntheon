# SPDX-License-Identifier: GPL-3.0-or-later
# Planted negatives for packaging/checks/crypto-symbols.py (AP-5 M2): small
# archives that look like a libcrypto with one hardware path missing. Built
# with GCC or Clang (the asm .zero makes a big fake function); never installed.
#   stub_aes:  a real-sized SHA-NI entry point, but perlasm-style stub GCM
#              entry points (a few bytes each) and the AVX-512 GCM fallback
#              (seven aliases on one unsized ud2)
#   c_sha:     a real-sized GCM entry point, but sha256_block_data_order from
#              a C object (sha256.c), not the SHA-NI module
add_library(planted_crypto_stub_aes STATIC stub_aes/sha256-x86_64.c stub_aes/aesni-gcm-x86_64.c stub_aes/aes-gcm-avx512.c)
add_library(planted_crypto_c_sha STATIC c_sha/sha256.c c_sha/aesni-gcm-x86_64.c)
foreach(t planted_crypto_stub_aes planted_crypto_c_sha)
    target_compile_options(${t} PRIVATE -O2 -w)
endforeach()
