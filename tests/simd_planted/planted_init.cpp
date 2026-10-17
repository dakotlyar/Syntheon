// SPDX-License-Identifier: GPL-3.0-or-later
// A planted wide-arch TU whose only fault is a dynamic initialiser (for the
// simd_odr_rejects_planted_initialiser CTest): everything is inside its
// namespace, but the static constructor would run before main(), before any
// dispatch, on every CPU.
namespace sb::cpu::simd_planted {

namespace {
int computed(int x)
{
    return x * 3 + 1;
}
volatile int ready = computed(14);  // not a constant: initialised at run time
} // namespace

} // namespace sb::cpu::simd_planted

extern "C" int sb_simd_planted_unit()
{
    return sb::cpu::simd_planted::ready;
}
