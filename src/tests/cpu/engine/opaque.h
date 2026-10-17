// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// opaque(x) (Phase 2 design §6.1): hides a value from the optimiser, so it
// can't constant-fold a kernel unit or drop work whose result it thinks is
// known. Applied to the seed and the buffer pointers on the way into a unit.
//
//   GCC / Clang  an empty asm that claims to read and modify x (in a register
//                or in memory)
//   MSVC         (no inline asm on x64 / ARM64) a write and a read back
//                through a volatile, between compiler-only fences
//                (std::atomic_signal_fence; MSVC deprecates _ReadWriteBarrier,
//                the intrinsic for the same thing)

#include <atomic>
#include <type_traits>

namespace sb::cpu {

template <typename T>
inline void opaque(T &value)
{
    static_assert(std::is_scalar_v<T>, "opaque() is for scalars and pointers");
#if defined(__clang__)
    asm volatile("" : "+r,m"(value) : : "memory");
#elif defined(__GNUC__)
    // GCC rejects "+r,m" ("impossible constraint") where the memory
    // alternative must come first; the meaning is the same.
    asm volatile("" : "+m,r"(value) : : "memory");
#else
    volatile T sink = value;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    value = sink;
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// The value, laundered: `opaqueValue(seed)` where an expression is handier.
template <typename T>
inline T opaqueValue(T value)
{
    opaque(value);
    return value;
}

} // namespace sb::cpu
