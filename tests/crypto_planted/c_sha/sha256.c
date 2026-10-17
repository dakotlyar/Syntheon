// SPDX-License-Identifier: GPL-3.0-or-later
// Planted: the C SHA-256 of a no-asm build (in sha256.c, not the SHA-NI module).
void sha256_block_data_order(void) { __asm__ volatile(".zero 1024"); }
