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
// One unit = copy the seed's pristine input into the thread's own buffer,
// then sort it (the copy is timed too: about 2% of the unit). The pristine
// inputs of all 64 seeds are made once per kernel and shared read-only by the
// threads (L6: no per-thread copies of large inputs; about 66 MB in all).
// The checksum hashes the sorted output; golden/sort.inc holds it per seed,
// and tst_cpu_kernels checks the order against std::sort.

#include "tests/cpu/engine/kernel.h"

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

class SortInputs
{
public:
    SortInputs();
    std::span<const std::uint64_t> keys(std::uint64_t seed) const;
    std::span<const StrRef> strings(std::uint64_t seed) const;
    const char *arena() const { return m_arena.data(); }

private:
    std::vector<std::uint64_t> m_keys;  // kSeeds x kSortKeys
    std::vector<StrRef> m_refs;         // kSeeds x kSortStrings
    std::vector<char> m_arena;          // every string, then 8 zero bytes
};

// One unit: copies `pristine` into `work` (the same size), sorts it, returns
// the hash of the sorted output.
std::uint64_t sortKeysUnit(std::span<const std::uint64_t> pristine, std::uint64_t *work);
std::uint64_t sortStringsUnit(std::span<const StrRef> pristine, StrRef *work, const char *arena);

class SortKeysKernel : public Kernel
{
public:
    explicit SortKeysKernel(const SortInputs &inputs) : m_inputs(inputs) {}
    std::string name() const override { return "Integer sort"; }
    double workPerUnit() const override { return double(kSortKeys); }
    // P14: elements per cycle per thread (it does about 0.005).
    double maxWorkPerCyclePerThread() const override { return 0.1; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;

private:
    const SortInputs &m_inputs;
};

class SortStringsKernel : public Kernel
{
public:
    explicit SortStringsKernel(const SortInputs &inputs) : m_inputs(inputs) {}
    std::string name() const override { return "String sort"; }
    double workPerUnit() const override { return double(kSortStrings); }
    // P14: elements per cycle per thread (it does about 0.002).
    double maxWorkPerCyclePerThread() const override { return 0.05; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;

private:
    const SortInputs &m_inputs;
};

} // namespace sb::cpu
