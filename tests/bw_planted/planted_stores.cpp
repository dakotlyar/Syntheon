// SPDX-License-Identifier: GPL-3.0-or-later
// A planted x64 bandwidth object that breaks every store rule (Phase 3
// design D4, §14.1), for the bw_stores_rejects_* CTests: its "non-temporal"
// write uses plain stores and no SFENCE, its plain write uses non-temporal
// stores, and its plain copy calls memcpy. Read and copy_nt are correct, so
// each rule is caught on its own. An object library: never linked.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <emmintrin.h>

extern "C" {

std::uint64_t sb_bw_planted_read(const void *p, std::size_t bytes)
{
    __m128i a = _mm_setzero_si128();
    for (const char *at = static_cast<const char *>(p); at < static_cast<const char *>(p) + bytes; at += 16)
        a = _mm_add_epi64(a, _mm_load_si128(reinterpret_cast<const __m128i *>(at)));
    return std::uint64_t(_mm_cvtsi128_si64(a));
}

void sb_bw_planted_write_nt(void *p, std::size_t bytes, std::uint64_t base)
{
    const __m128i v = _mm_set1_epi64x(static_cast<long long>(base));
    for (char *at = static_cast<char *>(p); at < static_cast<char *>(p) + bytes; at += 16)
        _mm_store_si128(reinterpret_cast<__m128i *>(at), v);
}

void sb_bw_planted_write(void *p, std::size_t bytes, std::uint64_t base)
{
    const __m128i v = _mm_set1_epi64x(static_cast<long long>(base));
    for (char *at = static_cast<char *>(p); at < static_cast<char *>(p) + bytes; at += 16)
        _mm_stream_si128(reinterpret_cast<__m128i *>(at), v);
    _mm_sfence();
}

// Through a volatile pointer, so no compiler inlines it as rep movsb.
void *(*volatile sb_bw_planted_memcpy)(void *, const void *, std::size_t) = &std::memcpy;

void sb_bw_planted_copy(void *dst, const void *src, std::size_t bytes)
{
    sb_bw_planted_memcpy(dst, src, bytes);
}

void sb_bw_planted_copy_nt(void *dst, const void *src, std::size_t bytes)
{
    const char *from = static_cast<const char *>(src);
    char *to = static_cast<char *>(dst);
    for (std::size_t i = 0; i < bytes; i += 16)
        _mm_stream_si128(reinterpret_cast<__m128i *>(to + i), _mm_load_si128(reinterpret_cast<const __m128i *>(from + i)));
    _mm_sfence();
}
}
