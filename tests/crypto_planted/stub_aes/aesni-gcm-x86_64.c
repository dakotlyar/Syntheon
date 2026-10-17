// SPDX-License-Identifier: GPL-3.0-or-later
// Planted: what perlasm emits for an assembler too old for AES-NI GCM,
// "xor eax,eax; ret" entry points.
int aesni_gcm_encrypt(void) { return 0; }
int aesni_gcm_decrypt(void) { return 0; }
