// SPDX-License-Identifier: GPL-3.0-or-later
// Planted: a SHA-NI entry point of a real size (1 KiB of filler).
void sha256_block_data_order_shaext(void) { __asm__ volatile(".zero 1024"); }
void sha256_block_data_order(void) { __asm__ volatile(".zero 1024"); }
