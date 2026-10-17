// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The sorting kernels (Phase 2 design §5.6, P10): `sort.u64` and `sort.str`,
// in elements sorted per second. Our own introsort (median-of-3 pivot,
// Hoare partition, insertion sort under 24 elements, heapsort below a depth
// of 2 floor(log2 n)) and our own short-string comparator: no std::sort and
// no memcmp, so every platform runs the same code and sorts into the same
// order (also among equal strings).
//
//   u64     65,536 random 64-bit keys (512 KiB, L2-resident)
//   strings 32,768 strings of 4-24 letters, each a StrRef {big-endian first 8
//           bytes, offset, length} into a shared arena; most share 8-12 bytes
//           of prefix with others ("stems"), so comparisons go past the first
//           word, where the comparator takes 8 bytes at a time
//
// One unit = make the seed's input in the thread's own buffer from the
// seeded PRNG (Dave, 2026-10-17: generated in the unit, no shared pool of
// per-seed inputs, no copy), then sort it; the generation is timed too (about
// 3-5% of the unit). u64: 65,536 draws. Strings: 32,768 picks from the
// arena, which is the one thing shared read-only by every thread (the
// strings' text and one StrRef per arena string, about 4 MB). The checksum
// hashes the sorted output; golden/sort.inc holds it per seed, and
// tst_cpu_kernels checks the order against std::sort.

#include "tests/cpu/engine/kernel.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sb::cpu {

inline constexpr std::size_t kSortKeys = 65536;
inline constexpr std::size_t kSortStrings = 32768;

struct StrRef {
    std::uint64_t prefixBE;  // the first 8 bytes, big-endian, zero-padded
    std::uint32_t offset;    // into the arena
    std::uint16_t len;       // 4..24
};

// a < b for two strings of `arena` (lexicographic by bytes; a proper prefix
// sorts first). The arena has 8 bytes of padding at its end.
bool strLess(const StrRef &a, const StrRef &b, const char *arena);

// Our introsort, on a range.
void introsort(std::uint64_t *first, std::uint64_t *last);
void introsort(StrRef *first, StrRef *last, const char *arena);

// The strings every seed's input is drawn from: made once, shared read-only.
class SortArena
{
public:
    SortArena();
    const char *text() const { return m_text.data(); }  // every string, then 8 zero bytes
    const StrRef *refs() const { return m_refs.data(); }
    std::size_t size() const { return m_refs.size(); }

private:
    std::vector<char> m_text;
    std::vector<StrRef> m_refs;
};

// A seed's input, into `out` (kSortKeys / kSortStrings elements).
void makeSortKeys(std::uint64_t seed, std::uint64_t *out);
void makeSortStrings(std::uint64_t seed, const SortArena &arena, StrRef *out);

// One unit: makes the seed's input in `work`, sorts it, returns the hash of
// the sorted output.
std::uint64_t sortKeysUnit(std::uint64_t seed, std::uint64_t *work);
std::uint64_t sortStringsUnit(std::uint64_t seed, const SortArena &arena, StrRef *work);

class SortKeysKernel : public Kernel
{
public:
    SortKeysKernel() = default;
    std::string name() const override { return "Integer sort"; }
    double workPerUnit() const override { return double(kSortKeys); }
    // P14: elements per cycle per thread (it does about 0.005).
    double maxWorkPerCyclePerThread() const override { return 0.1; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;
};

class SortStringsKernel : public Kernel
{
public:
    explicit SortStringsKernel(const SortArena &arena) : m_arena(arena) {}
    std::string name() const override { return "String sort"; }
    double workPerUnit() const override { return double(kSortStrings); }
    // P14: elements per cycle per thread (it does about 0.002).
    double maxWorkPerCyclePerThread() const override { return 0.05; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;

private:
    const SortArena &m_arena;
};

} // namespace sb::cpu
