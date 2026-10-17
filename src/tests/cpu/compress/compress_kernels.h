// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The compression sub-test's kernels (Phase 2 design §5.4, P11): eight
// scored metrics, compress and decompress for each codec, in MB of
// uncompressed data per second, plus the single-thread mixed unit for
// `single.compress`.
//
//   CorpusData           the 32 MiB corpus (made once, shared read-only by
//                        every thread: never copied per thread, L6), its
//                        CRC-32 (golden::kCorpusCrc32), and each block's
//                        blockHash(), made with it
//   CompressKernel       a unit compresses block b = unit index mod 128;
//                        its compressed size is compared with
//                        golden/compress.inc, a mismatch counted as a
//                        warning, not an error (formula v1: ARM output isn't
//                        confirmed in CI)
//   compressCorpus()     the unscored prep step: all 128 blocks compressed in
//                        parallel, for the decompress units and the ratio
//   DecompressKernel     a unit decompresses block b of that set; the output's
//                        blockHash() must be the original block's (an Error
//                        if not; Dave, 2026-10-17: a 4-lane hash, not a
//                        CRC-32, which cost as much as LZ4's decompression,
//                        nor a byte compare with the shared corpus)
//   SingleCompressKernel one unit compresses block b with each codec in turn
//                        and decompresses that output (each checked by its
//                        hash, as above), each of the 8 calls
//                        timed on its own; the window's rate is the geometric
//                        mean of the 8 (bytes / that call's total time), so
//                        xz's slow compression doesn't decide it alone
//
// Rates are in bytes per second (the CPU test shows MB = 10^6 bytes).

#include "tests/cpu/compress/codec.h"
#include "tests/cpu/compress/corpus.h"
#include "tests/cpu/engine/kernel.h"
#include "tests/cpu/kernels/block_hash.h"

#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <vector>

namespace sb::cpu {

struct CorpusData {
    std::vector<std::uint8_t> bytes;                  // kCorpusSize
    std::array<std::uint32_t, kCorpusBlocks> blockCrc{};  // zlib crc32() of each block (for crc)
    std::array<std::uint64_t, kCorpusBlocks> blockHash{};  // blockHash() of each block
    std::uint32_t crc = 0;                            // of the whole corpus (golden::kCorpusCrc32)
    std::span<const std::uint8_t> block(std::size_t b) const
    {
        return std::span(bytes).subspan(b * kCorpusBlockSize, kCorpusBlockSize);
    }
};
// Generates the corpus on `threads` threads and checksums it.
CorpusData makeCorpusData(int threads);

// The golden compressed size of block `b` with `codec` (golden/compress.inc).
std::uint32_t goldenCompressedSize(Codec codec, std::size_t b);

// The compressed corpus for one codec.
struct CompressedSet {
    Codec codec = Codec::Zlib;
    std::vector<std::vector<std::uint8_t>> blocks;  // kCorpusBlocks
    std::uint64_t compressedBytes = 0;
    int sizeMismatches = 0;  // blocks whose size isn't the golden one
    // Uncompressed / compressed, the compress.<codec>.ratio metric (2.5 = 40%).
    double ratio() const { return compressedBytes ? double(kCorpusSize) / double(compressedBytes) : 0; }
};
// Compresses every block on `threads` threads. `cancelled` is polled every
// 50 ms (the codecs check between 64 KiB pieces); nothing if it said so. The
// threads are joined before it returns. A codec failure is rethrown.
std::optional<CompressedSet> compressCorpus(Codec codec, const CorpusData &corpus, int threads,
                                            const std::function<bool()> &cancelled);

class CompressKernel : public Kernel
{
public:
    CompressKernel(Codec codec, const CorpusData &corpus);
    std::string name() const override;
    double workPerUnit() const override { return double(kCorpusBlockSize); }
    double maxWorkPerCyclePerThread() const override;
    std::uint64_t seeds() const override { return kCorpusBlocks; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    // Units whose compressed size wasn't the golden one (a warning).
    int sizeMismatches() const { return m_sizeMismatches.load(); }

private:
    Codec m_codec;
    const CorpusData &m_corpus;
    mutable std::atomic<int> m_sizeMismatches{0};
};

class DecompressKernel : public Kernel
{
public:
    DecompressKernel(const CorpusData &corpus, const CompressedSet &set);
    std::string name() const override;
    double workPerUnit() const override { return double(kCorpusBlockSize); }
    double maxWorkPerCyclePerThread() const override;
    std::uint64_t seeds() const override { return kCorpusBlocks; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;

private:
    const CorpusData &m_corpus;
    const CompressedSet &m_set;
};

class SingleCompressKernel : public Kernel
{
public:
    explicit SingleCompressKernel(const CorpusData &corpus);
    std::string name() const override { return "Single-thread compression"; }
    // Bytes through the 8 calls; the window's rate comes from the kernel
    // (the geometric mean), not from this.
    double workPerUnit() const override { return double(2 * kCodecCount * kCorpusBlockSize); }
    // P14: the geometric mean can't exceed the fastest call's rate; 16 bytes
    // per cycle covers LZ4 decompression with room to spare.
    double maxWorkPerCyclePerThread() const override { return 16; }
    std::uint64_t seeds() const override { return kCorpusBlocks; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;
    int sizeMismatches() const { return m_sizeMismatches.load(); }

private:
    const CorpusData &m_corpus;
    mutable std::atomic<int> m_sizeMismatches{0};
};

} // namespace sb::cpu
