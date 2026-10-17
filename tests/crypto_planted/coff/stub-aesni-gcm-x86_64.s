# SPDX-License-Identifier: GPL-3.0-or-later
# Planted (COFF): perlasm's stub AES-NI GCM entry points, a few bytes each.
.text
.globl aesni_gcm_encrypt
aesni_gcm_encrypt:
	xor %eax,%eax
	ret
.globl aesni_gcm_decrypt
aesni_gcm_decrypt:
	xor %eax,%eax
	ret
