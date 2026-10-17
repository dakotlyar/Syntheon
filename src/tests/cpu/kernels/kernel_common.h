// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What every kernel TU shares (Phase 2 design §5, §6.1, §6.2, P7).
//
// The kernel TUs are compiled at -O2 / /O2 in every build type, without
// auto-vectorisation, without FP contraction and without LTO/IPO
// (src/tests/cpu/kernels/CMakeLists.txt; the cpu_kernel_flags CTest checks
// the flags in compile_commands.json). GCC and Clang get that from the
// command line. MSVC has no switch for either, so each kernel TU also starts
// with `#pragma fp_contract(off)` (SB_KERNEL_TU below) and puts SB_NO_VECTOR
// before its hot loops. MSVC's loop pragma only takes a classic for / while /
// do loop, not a range-based for (error C3925): the cpu_kernel_flags CTest
// fails on an SB_NO_VECTOR followed by anything else.
//
// Entry points are SB_NOINLINE, so a unit is one real call the optimiser
// can't merge into the caller, and its inputs go through opaque().

#if defined(_MSC_VER)
#define SB_NOINLINE __declspec(noinline)
#define SB_NO_VECTOR __pragma(loop(no_vector))
#define SB_KERNEL_TU __pragma(fp_contract(off))
#else
#define SB_NOINLINE __attribute__((noinline))
#define SB_NO_VECTOR
#define SB_KERNEL_TU
#endif

#include <cstdint>

namespace sb::cpu {

// Folds a value into a checksum: odd multiplier, so no bit is lost mod 2^64.
constexpr std::uint64_t foldChecksum(std::uint64_t sum, std::uint64_t value)
{
    return (sum ^ value) * 0x9E3779B97F4A7C15ull + 0x7F4A7C15ull;
}

} // namespace sb::cpu
