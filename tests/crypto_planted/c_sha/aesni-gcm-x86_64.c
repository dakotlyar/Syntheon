// SPDX-License-Identifier: GPL-3.0-or-later
// Planted: a GCM entry point of a real size (2 KiB of filler).
void aesni_gcm_encrypt(void) { __asm__ volatile(".zero 2048"); }
void aesni_gcm_decrypt(void) { __asm__ volatile(".zero 2048"); }
