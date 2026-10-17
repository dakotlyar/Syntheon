// SPDX-License-Identifier: GPL-3.0-or-later
// A planted wide-arch TU that breaks the SIMD ODR rule (simd_common.h), for
// the simd_odr_rejects_planted CTest: built with the AVX2 / NEON-baseline
// flags like a real path, it defines an inline function and a std:: template
// instantiation outside its namespace, and their addresses escape, so every
// compiler emits them (weak on ELF, COMDAT on COFF).
#include <algorithm>

inline float plantedOutside(float x)
{
    return x * 2.0f + 1.0f;
}

extern "C" {
float (*sb_simd_planted_outside)(float) = &plantedOutside;
const float &(*sb_simd_planted_min)(const float &, const float &) = &std::min<float>;
void sb_simd_planted_unit() {}
}
