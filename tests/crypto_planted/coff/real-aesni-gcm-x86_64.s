# SPDX-License-Identifier: GPL-3.0-or-later
# Planted (COFF): real-sized AES-NI GCM entry points; aesni_gcm_encrypt is the
# last global of .text (measured to the section end), and a global in .rdata
# after it must not cut it short.
.text
.def _aesni_ctr32_6x; .scl 3; .type 32; .endef
_aesni_ctr32_6x:
	.zero 300
.globl aesni_gcm_decrypt
.def aesni_gcm_decrypt; .scl 2; .type 32; .endef
aesni_gcm_decrypt:
	.zero 600
.globl aesni_gcm_encrypt
.def aesni_gcm_encrypt; .scl 2; .type 32; .endef
aesni_gcm_encrypt:
	.zero 1000
.section .rdata,"dr"
.globl some_table
some_table:
	.zero 8
