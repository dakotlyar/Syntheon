// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/compress/compress_kernels.h"

#include "tests/cpu/engine/worker_pool.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/kernel_common.h"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>

namespace sb::cpu {

namespace {

std::size_t codecIndex(Codec c)
{
    return std::size_t(c);
}

// P14, bytes per cycle per thread: several times the fastest CPU's rate for
// each call (Knowhere: LZ4 decompression about 1 byte per cycle).
double compressCeiling(Codec c)
{
    switch (c) {
    case Codec::Zlib: return 1;
    case Codec::Zstd: return 2;
    case Codec::Lz4: return 4;
    case Codec::Xz: return 0.5;
    }
    return 1;
}
double decompressCeiling(Codec c)
{
    switch (c) {
    case Codec::Zlib: return 4;
    case Codec::Zstd: return 8;
    case Codec::Lz4: return 16;
    case Codec::Xz: return 2;
    }
    return 4;
}

std::uint32_t crcOf(std::span<const std::uint8_t> data)
{
    return std::uint32_t(crc32(0L, data.data(), uInt(data.size())));
}

// §5.4 (Dave, 2026-10-17): a decompressed block is compared with the
// original byte for byte (not by CRC-32, which cost as much as LZ4's
// decompression itself). 0 if they are equal, else 1 + the block number.
std::uint64_t mismatch(std::span<const std::uint8_t> got, std::span<const std::uint8_t> original, std::size_t b)
{
    const bool same = got.size() == original.size() && std::memcmp(got.data(), original.data(), got.size()) == 0;
    return same ? 0 : 1 + b;
}

} // namespace

CorpusData makeCorpusData(int threads)
{
    CorpusData c;
    c.bytes = generateCorpus(threads);
    c.crc = std::uint32_t(crc32(0L, Z_NULL, 0));
    for (std::size_t b = 0; b < kCorpusBlocks; ++b) {
        c.blockCrc[b] = crcOf(c.block(b));
        c.crc = std::uint32_t(crc32_combine(c.crc, c.blockCrc[b], z_off_t(kCorpusBlockSize)));
    }
    return c;
}

std::uint32_t goldenCompressedSize(Codec codec, std::size_t b)
{
    return golden::kCompressedSizes[codecIndex(codec)][b % kCorpusBlocks];
}

std::optional<CompressedSet> compressCorpus(Codec codec, const CorpusData &corpus, int threads,
                                            const std::function<bool()> &cancelled)
{
    CompressedSet set;
    set.codec = codec;
    set.blocks.resize(kCorpusBlocks);
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> finished{0};
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::exception_ptr failure;
    const auto work = [&] {
        try {
            CodecContext ctx(codec);
            std::vector<std::uint8_t> out(CodecContext::bound(codec, kCorpusBlockSize));
            for (std::size_t b; (b = next++) < kCorpusBlocks && !cancel.load();) {
                const std::optional<std::size_t> n = ctx.compress(corpus.block(b), out, &cancel);
                if (!n)
                    break;
                set.blocks[b].assign(out.begin(), out.begin() + std::ptrdiff_t(*n));
                ++finished;
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            if (!failure)
                failure = std::current_exception();
            cancel = true;
        }
    };
    const int n = std::clamp(threads, 1, int(kCorpusBlocks));
    std::vector<std::thread> pool;
    for (int t = 0; t < n; ++t)
        pool.emplace_back(work);
    // The coordinator: polls for a cancel every 50 ms (§4.1) until done.
    while (finished.load() < kCorpusBlocks && !cancel.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (cancelled && cancelled())
            cancel = true;
    }
    for (std::thread &t : pool)
        t.join();
    if (failure)
        std::rethrow_exception(failure);
    if (finished.load() < kCorpusBlocks)
        return std::nullopt;
    for (std::size_t b = 0; b < kCorpusBlocks; ++b) {
        set.compressedBytes += set.blocks[b].size();
        if (set.blocks[b].size() != goldenCompressedSize(codec, b))
            ++set.sizeMismatches;
    }
    return set;
}

// --- compression -------------------------------------------------------------

CompressKernel::CompressKernel(Codec codec, const CorpusData &corpus) : m_codec(codec), m_corpus(corpus) {}

std::string CompressKernel::name() const
{
    return std::string(codecName(m_codec)) + " compression";
}

double CompressKernel::maxWorkPerCyclePerThread() const
{
    return compressCeiling(m_codec);
}

std::unique_ptr<KernelThread> CompressKernel::makeThread(int) const
{
    struct T : KernelThread {
        const CompressKernel *k;
        CodecContext ctx;
        std::vector<std::uint8_t> out;
        T(const CompressKernel *kernel, Codec c)
            : k(kernel), ctx(c), out(CodecContext::bound(c, kCorpusBlockSize), 0)  // touched here
        {
        }
        std::uint64_t unit(std::uint64_t unitIndex) override
        {
            const std::size_t b = std::size_t(unitIndex % kCorpusBlocks);
            const std::optional<std::size_t> n = ctx.compress(k->m_corpus.block(b), out, cancelFlag());
            if (!n)
                return 0;  // cancelled: discarded by the engine
            if (*n != goldenCompressedSize(ctx.codec(), b))
                ++k->m_sizeMismatches;
            return *n;
        }
    };
    return std::make_unique<T>(this, m_codec);
}

// --- decompression -----------------------------------------------------------

DecompressKernel::DecompressKernel(const CorpusData &corpus, const CompressedSet &set) : m_corpus(corpus), m_set(set) {}

std::string DecompressKernel::name() const
{
    return std::string(codecName(m_set.codec)) + " decompression";
}

double DecompressKernel::maxWorkPerCyclePerThread() const
{
    return decompressCeiling(m_set.codec);
}

std::unique_ptr<KernelThread> DecompressKernel::makeThread(int) const
{
    struct T : KernelThread {
        const CompressedSet *set;
        const CorpusData *corpus;
        CodecContext ctx;
        std::vector<std::uint8_t> out = std::vector<std::uint8_t>(kCorpusBlockSize, 0);  // touched here
        T(const CompressedSet *s, const CorpusData *c) : set(s), corpus(c), ctx(s->codec) {}
        std::uint64_t unit(std::uint64_t unitIndex) override
        {
            const std::size_t b = std::size_t(unitIndex % kCorpusBlocks);
            ctx.decompress(set->blocks[b], out);
            // §5.4: every decompressed block against the original, byte for byte.
            return mismatch(out, corpus->block(b), b);
        }
    };
    return std::make_unique<T>(&m_set, &m_corpus);
}

std::optional<std::uint64_t> DecompressKernel::golden(std::uint64_t) const
{
    return 0;  // no block differs from its original
}

// --- the single-thread mixed unit ---------------------------------------------

SingleCompressKernel::SingleCompressKernel(const CorpusData &corpus) : m_corpus(corpus) {}

std::unique_ptr<KernelThread> SingleCompressKernel::makeThread(int) const
{
    struct T : KernelThread {
        const SingleCompressKernel *k;
        std::vector<std::unique_ptr<CodecContext>> ctx;
        std::vector<std::uint8_t> packed, unpacked;
        // Per call (codec x compress / decompress): bytes and seconds in this window.
        std::array<double, 2 * kCodecCount> bytes{}, seconds{};
        explicit T(const SingleCompressKernel *kernel) : k(kernel)
        {
            std::size_t most = 0;
            for (const Codec c : kCodecs) {
                ctx.push_back(std::make_unique<CodecContext>(c));
                most = std::max(most, CodecContext::bound(c, kCorpusBlockSize));
            }
            packed.assign(most, 0);
            unpacked.assign(kCorpusBlockSize, 0);
        }
        void beginWindow() override
        {
            bytes.fill(0);
            seconds.fill(0);
        }
        std::optional<double> windowRate() const override
        {
            double logSum = 0;
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                if (!(seconds[i] > 0) || !(bytes[i] > 0))
                    return std::nullopt;
                logSum += std::log(bytes[i] / seconds[i]);
            }
            return std::exp(logSum / double(bytes.size()));
        }
        std::uint64_t unit(std::uint64_t unitIndex) override
        {
            const std::size_t b = std::size_t(unitIndex % kCorpusBlocks);
            const std::span<const std::uint8_t> block = k->m_corpus.block(b);
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < kCodecCount; ++i) {
                CodecContext &c = *ctx[i];
                const auto t0 = Clock::now();
                const std::optional<std::size_t> n = c.compress(block, packed, cancelFlag());
                const auto t1 = Clock::now();
                if (!n)
                    return 0;  // cancelled: discarded by the engine
                c.decompress(std::span(packed).first(*n), unpacked);
                const auto t2 = Clock::now();
                seconds[2 * i] += std::chrono::duration<double>(t1 - t0).count();
                seconds[2 * i + 1] += std::chrono::duration<double>(t2 - t1).count();
                bytes[2 * i] += double(kCorpusBlockSize);
                bytes[2 * i + 1] += double(kCorpusBlockSize);
                if (*n != goldenCompressedSize(c.codec(), b))
                    ++k->m_sizeMismatches;
                sum += mismatch(unpacked, block, b);
            }
            return sum;
        }
    };
    return std::make_unique<T>(this);
}

std::optional<std::uint64_t> SingleCompressKernel::golden(std::uint64_t) const
{
    return 0;  // all four round trips gave the original block
}

} // namespace sb::cpu
