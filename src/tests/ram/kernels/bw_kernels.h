// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The bandwidth kernels (Phase 3 design §5.3, §9, P8, P9, P10, D4, D5):
// read, write and copy over per-thread slices far larger than the caches.
//
// Paths: AVX2 where the CPU and the OS have it, else SSE2 (x64); NEON
// (ARM64). No AVX-512: DRAM bandwidth is bound by outstanding misses, not
// vector width. x64 measures write and copy with both store types (D4):
// non-temporal + SFENCE (scored) and plain (shown, not scored); ARM64 with
// plain stores only (scored). bandwidthVariants() lists them in §3's order.
//
// Data and checks. Every slice is a whole number of 1 MiB cells, and every
// cell always holds an arithmetic pattern: word i of cell c is V[c] + i,
// with V[c] kept in the slice's table. fill() writes it at first touch;
// a write unit stores base + i over its block (so each cell of it gets a
// new V), and a copy unit moves whole cells (their V moves with them). So:
//   - a read unit's sum is known exactly from the table (§9, "golden by
//     construction"), and a wrong sum is an Error;
//   - after each write and copy unit one 64-byte line per MiB is compared
//     with the pattern (the line differs from unit to unit), so a skipped
//     or misplaced block is an Error. §5.3 samples after each window; the
//     check runs after every unit instead, inside its timed span: 16 lines
//     against 16 MiB of streaming (well under 0.1% of the unit).
// A failed check throws from the unit: the engine ends the measurement
// with the message as its error (the sub-test is an Error, §9).
//
// Units (§5.3): a block of B = min(16 MiB, slice) bytes, cycling through
// the thread's slices. Read and write: B bytes per unit. Copy: from the
// slice's first half to its second half, B / 2 per unit, counted as
// B / 2 read + B / 2 written = B (P10). So workPerUnit() is B for all
// three, in bytes; GB/s = rate / 1e9.
//
// The copy's source is rotated (AP-5 M2): the first half never changes, so
// a destination copied from the same place again would already hold what
// the check expects, and a copy that did nothing would pass. Each time a
// destination chunk is copied into, its source starts one 1 MiB cell further
// round the first half than the last time (wrapping at the half's end, in
// up to two calls), so what it must hold now differs from what it holds.
// That needs at least two cells in a half (a slice of 4 MiB or more): a
// 2 MiB slice's copies are only checked the first time.
//
// Threads: an all-thread pool has one slice per worker (worker i reads and
// writes slice i, which it first touched: §4.2 NUMA); the single-thread
// pool (one worker) cycles through every slice.

#include "isa/cache_topology.h"
#include "isa/cpu_features.h"
#include "tests/cpu/engine/kernel.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sb::ram {

inline constexpr std::size_t kBwCell = std::size_t(1) << 20;
inline constexpr std::size_t kBwBlock = std::size_t(16) << 20;
inline constexpr std::size_t kBwLine = 64;
// The slice granularity (P9: 2 MiB multiples, so a copy's halves are
// whole cells).
inline constexpr std::size_t kBwSliceUnit = 2 * kBwCell;

enum class BwPath { Sse2, Avx2, Neon };
enum class BwOp { Read, Write, Copy };
enum class BwStores { Plain, NonTemporal };

struct BwVariant {
    BwOp op = BwOp::Read;
    BwStores stores = BwStores::Plain;  // reads: Plain (no stores)
    bool scored = true;
    // The metric key's tail: "read", "write", "copy", "write.plain",
    // "copy.plain" (the RAM test puts "bandwidth." or "bandwidth.st." before it).
    std::string key;
};

// AVX2 when the CPU and the OS support it (CpuFeatures' Avx2 includes the
// OS state), else SSE2; NEON on ARM64.
BwPath chooseBandwidthPath(const isa::CpuFeatures &features);
// `bandwidth.path`'s text (§2).
std::string bandwidthPathLabel(BwPath path);
// The measurements of one bandwidth step, in §3's order: x64 read, write
// (NT, scored), copy (NT, scored), write (plain), copy (plain); ARM64 read,
// write, copy (plain, scored).
std::vector<BwVariant> bandwidthVariants(BwPath path);

// The path's entry points; nullptr for a variant the path doesn't have
// (ARM64 non-temporal) or a path this build has no code for (the other
// architecture).
struct BwFunctions {
    std::uint64_t (*read)(const void *, std::size_t) = nullptr;
    void (*write)(void *, std::size_t, std::uint64_t) = nullptr;
    void (*writeNt)(void *, std::size_t, std::uint64_t) = nullptr;
    void (*copy)(void *, const void *, std::size_t) = nullptr;
    void (*copyNt)(void *, const void *, std::size_t) = nullptr;
};
BwFunctions bandwidthFunctions(BwPath path);

// One thread's memory: `bytes` (a multiple of kBwSliceUnit) at a 64-byte
// aligned `data`, owned elsewhere (a HugeBuffer), and its pattern table.
class BandwidthSlice
{
public:
    BandwidthSlice(std::byte *data, std::size_t bytes);

    std::byte *data() const { return m_data; }
    std::size_t bytes() const { return m_bytes; }

    // First touch (§5.3, §6.1 step 3): writes the pattern over the whole
    // slice, on the calling thread (its owner), in 64 MiB chunks with a
    // cancel check before each. False if cancelled (the table then says
    // what was written; the rest isn't checked until written again).
    bool fill(std::uint64_t seed, const std::function<bool()> &cancelled);

    // The exact sum of the words in [offset, offset + length) (whole cells).
    std::uint64_t expectedSum(std::size_t offset, std::size_t length) const;
    // One line per cell of [offset, offset + length) against the table's
    // pattern, the line within each cell chosen by `salt`.
    bool verifySample(std::size_t offset, std::size_t length, std::uint64_t salt) const;
    // The same for a copy's destination, against the source cells' pattern
    // (before copied() moves it in the table).
    bool verifyCopySample(std::size_t dstOffset, std::size_t srcOffset, std::size_t length, std::uint64_t salt) const;
    // The table after a write of base + i over [offset, offset + length),
    // or a copy of [src, src + length) to [dst, dst + length).
    void wrote(std::size_t offset, std::size_t length, std::uint64_t base);
    void copied(std::size_t dstOffset, std::size_t srcOffset, std::size_t length);
    // The rotation (in cells) for the next copy into the second-half chunk
    // at `dstOffset`: one more than the last one for that chunk, modulo the
    // half's cells. Kept in the slice, so it carries on across measurements.
    std::size_t nextCopyShift(std::size_t dstOffset);

private:
    std::byte *m_data;
    std::size_t m_bytes;
    std::vector<std::uint64_t> m_cellBase;  // V[c]
    // Cells the table can vouch for (false only after a cancelled fill).
    std::vector<bool> m_known;
    // Per second-half cell: the last copy rotation into the chunk starting there.
    std::vector<std::uint32_t> m_copyShift;
};

// P15: a thread above 128 bytes per cycle at 7 GHz is implausible; the
// engine's P14 ceiling multiplies by 1.5, so the per-cycle figure is
// 128 / 1.5. The same per thread for read, write and copy (copy's counted
// bytes, P10).
inline constexpr double kBwMaxBytesPerCycle = 128.0 / 1.5;

class BandwidthKernel : public cpu::Kernel
{
public:
    // `slices` all the same size; `threads` 1 (every slice, in order) or
    // slices.size() (one each). The slices must have been filled.
    BandwidthKernel(BwPath path, BwVariant variant, std::vector<BandwidthSlice *> slices, int threads);
    // The same with explicit entry points (tests plant broken ones).
    BandwidthKernel(const BwFunctions &functions, BwVariant variant, std::vector<BandwidthSlice *> slices, int threads);
    std::string name() const override { return m_name; }
    double workPerUnit() const override { return double(m_block); }
    double maxWorkPerCyclePerThread() const override { return kBwMaxBytesPerCycle; }
    std::unique_ptr<cpu::KernelThread> makeThread(int thread) const override;

    std::size_t blockBytes() const { return m_block; }

private:
    BwVariant m_variant;
    BwFunctions m_fn;
    std::vector<BandwidthSlice *> m_slices;
    int m_threads;
    std::size_t m_block;
    std::string m_name;
};

// The machine's last-level caches, from every pinned CPU's topology: each
// distinct instance (by the CPUs that share it) counted once in the total
// (Tiamat: 32 + 96 MiB = 128 MiB), and the largest single one (96 MiB).
struct LastLevelCaches {
    std::uint64_t totalBytes = 0;
    std::uint64_t largestBytes = 0;
    int largestLevel = 0;  // its level (3 on most machines; 2 on an L3-less core)
};
LastLevelCaches lastLevelCaches(const std::vector<isa::CacheTopology> &perCpu);

// P9, as Dave set it on 2026-10-19 (AP-5 checkpoint 2: a 32 MiB slice per
// thread was smaller than Tiamat's 96 MiB V-cache L3, and an SMT pair's
// slices settled in it, reading above the DRAM peak):
//   - total wanted = max(1 GiB, 8 x the total last-level cache), split into
//     one slice per thread: a multiple of 2 MiB, and of 16 MiB (the block)
//     once a slice is larger than that, so every unit is the same size;
//   - and every slice at least the largest single last-level cache any
//     pinned CPU sits behind, rounded up to 16 MiB (the floor; Tiamat:
//     96 MiB x 32 threads = 3 GiB);
//   - at most 25% of installed physical memory in total (unknown memory:
//     4 GiB). If even the floor doesn't fit, the run goes ahead at the cap
//     (slices rounded down: 16 MiB multiples, at least 2 MiB) and `note`
//     says bandwidth may partly come from cache.
// The load generator streams over the same slices, so the floor holds there.
struct BandwidthPlan {
    int threads = 1;
    std::size_t sliceBytes = 0;
    std::size_t blockBytes = 0;
    std::size_t floorBytes = 0;  // the per-thread floor (0: no cache known)
    bool limited = false;        // the 25% cap cut a slice below the floor
    std::string note;            // the warn note when limited (empty otherwise)
    std::uint64_t totalBytes() const { return std::uint64_t(sliceBytes) * std::uint64_t(threads); }
    // `bandwidth.buffer`'s text: "3 GiB in total (96 MiB per thread)",
    // "1152 MiB in total (48 MiB per thread)".
    std::string describe() const;
};
BandwidthPlan planBandwidth(const LastLevelCaches &caches, int threads, std::uint64_t physicalMemoryBytes);

// D18: the RAM test's peak footprint, for probe()'s free-memory warning
// (free memory under this + 512 MiB): the latency buffer (the DRAM target)
// plus the bandwidth slices; Tiamat: 1 GiB + 3 GiB.
std::uint64_t ramPeakFootprint(std::uint64_t latencyBufferBytes, const BandwidthPlan &plan);

} // namespace sb::ram
