// SPDX-License-Identifier: GPL-3.0-or-later
// Planted: OpenSSL 3.5.9's old-assembler fallback for the AVX-512 GCM code
// (crypto/modes/asm/aes-gcm-avx512.pl): seven names on one "ud2; ret", only
// the first with a .size. It once passed the check as "size unknown".
__asm__(".text\n"
        ".globl ossl_vaes_vpclmulqdq_capable\n"
        ".type ossl_vaes_vpclmulqdq_capable,@function\n"
        "ossl_vaes_vpclmulqdq_capable:\n"
        "    xor %eax,%eax\n"
        "    ret\n"
        ".size ossl_vaes_vpclmulqdq_capable, .-ossl_vaes_vpclmulqdq_capable\n"
        ".globl ossl_aes_gcm_init_avx512\n"
        ".globl ossl_aes_gcm_setiv_avx512\n"
        ".globl ossl_aes_gcm_update_aad_avx512\n"
        ".globl ossl_aes_gcm_encrypt_avx512\n"
        ".globl ossl_aes_gcm_decrypt_avx512\n"
        ".globl ossl_aes_gcm_finalize_avx512\n"
        ".globl ossl_gcm_gmult_avx512\n"
        ".type ossl_aes_gcm_init_avx512,@function\n"
        "ossl_aes_gcm_init_avx512:\n"
        "ossl_aes_gcm_setiv_avx512:\n"
        "ossl_aes_gcm_update_aad_avx512:\n"
        "ossl_aes_gcm_encrypt_avx512:\n"
        "ossl_aes_gcm_decrypt_avx512:\n"
        "ossl_aes_gcm_finalize_avx512:\n"
        "ossl_gcm_gmult_avx512:\n"
        "    .byte 0x0f,0x0b\n"
        "    ret\n"
        ".size ossl_aes_gcm_init_avx512, .-ossl_aes_gcm_init_avx512\n");
