// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/kernels/bw_kernels.h"

#include "tests/cpu/engine/opaque.h"
#include "tests/cpu/kernels/prng.h"
#include "tests/ram/kernels/bw/bw_common.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace sb::ram {

namespace {

constexpr std::size_t kCellWords = kBwCell / sizeof(std::uint64_t);
constexpr std::size_t kCellLines = kBwCell / kBwLine;
constexpr std::size_t kFillChunkCells = 64;  // 64 MiB between cancel checks
constexpr std::uint64_t MiB = 1024 * 1024, GiB = 1024 * MiB;

#if defined(__x86_64__) || defined(_M_X64)
constexpr bool kX64 = true;
#else
constexpr bool kX64 = false;
#endif

std::uint64_t mix(std::uint64_t a, std::uint64_t b)
{
    return cpu::SplitMix64(a * 0x9E3779B97F4A7C15ull + b).next();
}

// Sum of V + i for i < n, mod 2^64.
constexpr std::uint64_t cellSum(std::uint64_t v)
{
    return v * kCellWords + std::uint64_t(kCellWords) * (kCellWords - 1) / 2;
}

bool lineHolds(const std::byte *line, std::uint64_t first)
{
    const auto *w = reinterpret_cast<const std::uint64_t *>(line);
    for (std::size_t i = 0; i < kBwLine / sizeof(std::uint64_t); ++i) {
        if (w[i] != first + i)
            return false;
    }
    return true;
}

std::string opName(const BwVariant &v)
{
    switch (v.op) {
    case BwOp::Read:
        return "Read bandwidth";
    case BwOp::Write:
        return v.stores == BwStores::NonTemporal ? "Write bandwidth (non-temporal stores)"
                                                 : "Write bandwidth (plain stores)";
    case BwOp::Copy:
        return v.stores == BwStores::NonTemporal ? "Copy bandwidth (non-temporal stores)"
                                                 : "Copy bandwidth (plain stores)";
    }
    return "Bandwidth";
}

std::size_t roundUp(std::size_t v, std::size_t to)
{
    return (v + to - 1) / to * to;
}

} // namespace

BwPath chooseBandwidthPath(const isa::CpuFeatures &features)
{
    if constexpr (!kX64)
        return BwPath::Neon;
    return features.has(isa::Feature::Avx2) ? BwPath::Avx2 : BwPath::Sse2;
}

std::string bandwidthPathLabel(BwPath path)
{
    switch (path) {
    case BwPath::Avx2:
        return "AVX2 loads; write/copy scored with non-temporal stores, plain stores also shown";
    case BwPath::Sse2:
        return "SSE2 loads; write/copy scored with non-temporal stores, plain stores also shown";
    case BwPath::Neon:
        return "NEON loads and plain stores";
    }
    return {};
}

std::vector<BwVariant> bandwidthVariants(BwPath path)
{
    if (path == BwPath::Neon) {
        return {{BwOp::Read, BwStores::Plain, true, "read"},
                {BwOp::Write, BwStores::Plain, true, "write"},
                {BwOp::Copy, BwStores::Plain, true, "copy"}};
    }
    return {{BwOp::Read, BwStores::Plain, true, "read"},
            {BwOp::Write, BwStores::NonTemporal, true, "write"},
            {BwOp::Copy, BwStores::NonTemporal, true, "copy"},
            {BwOp::Write, BwStores::Plain, false, "write.plain"},
            {BwOp::Copy, BwStores::Plain, false, "copy.plain"}};
}

BwFunctions bandwidthFunctions(BwPath path)
{
    BwFunctions f;
#if defined(__x86_64__) || defined(_M_X64)
    if (path == BwPath::Avx2)
        f = {sb_bw_avx2_read, sb_bw_avx2_write, sb_bw_avx2_write_nt, sb_bw_avx2_copy, sb_bw_avx2_copy_nt};
    else if (path == BwPath::Sse2)
        f = {sb_bw_sse2_read, sb_bw_sse2_write, sb_bw_sse2_write_nt, sb_bw_sse2_copy, sb_bw_sse2_copy_nt};
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (path == BwPath::Neon)
        f = {sb_bw_neon_read, sb_bw_neon_write, nullptr, sb_bw_neon_copy, nullptr};
#else
    (void)path;
#endif
    return f;
}

// --- BandwidthSlice ---

BandwidthSlice::BandwidthSlice(std::byte *data, std::size_t bytes)
    : m_data(data), m_bytes(bytes), m_cellBase(bytes / kBwCell), m_known(bytes / kBwCell, false),
      m_copyShift(bytes / kBwCell / 2, 0)
{
    if (bytes == 0 || bytes % kBwSliceUnit != 0 || reinterpret_cast<std::uintptr_t>(data) % bw::kBwAlign != 0)
        throw std::invalid_argument("BandwidthSlice: a 64-byte aligned multiple of 2 MiB");
}

bool BandwidthSlice::fill(std::uint64_t seed, const std::function<bool()> &cancelled)
{
    const std::size_t cells = m_cellBase.size();
    for (std::size_t c = 0; c < cells; ++c) {
        if (c % kFillChunkCells == 0 && cancelled && cancelled())
            return false;
        const std::uint64_t v = mix(seed, c);
        auto *w = reinterpret_cast<std::uint64_t *>(m_data + c * kBwCell);
        for (std::size_t i = 0; i < kCellWords; ++i)
            w[i] = v + i;
        m_cellBase[c] = v;
        m_known[c] = true;
    }
    return true;
}

std::uint64_t BandwidthSlice::expectedSum(std::size_t offset, std::size_t length) const
{
    std::uint64_t sum = 0;
    for (std::size_t c = offset / kBwCell; c < (offset + length) / kBwCell; ++c)
        sum += cellSum(m_cellBase[c]);
    return sum;
}

bool BandwidthSlice::verifySample(std::size_t offset, std::size_t length, std::uint64_t salt) const
{
    return verifyCopySample(offset, offset, length, salt);
}

bool BandwidthSlice::verifyCopySample(std::size_t dstOffset, std::size_t srcOffset, std::size_t length,
                                      std::uint64_t salt) const
{
    for (std::size_t k = 0; k < length / kBwCell; ++k) {
        const std::size_t src = srcOffset / kBwCell + k;
        if (!m_known[src])
            continue;
        const std::size_t line = std::size_t(mix(salt, k) % kCellLines);
        const std::byte *at = m_data + dstOffset + k * kBwCell + line * kBwLine;
        if (!lineHolds(at, m_cellBase[src] + line * (kBwLine / sizeof(std::uint64_t))))
            return false;
    }
    return true;
}

void BandwidthSlice::wrote(std::size_t offset, std::size_t length, std::uint64_t base)
{
    for (std::size_t k = 0; k < length / kBwCell; ++k) {
        m_cellBase[offset / kBwCell + k] = base + std::uint64_t(k) * kCellWords;
        m_known[offset / kBwCell + k] = true;
    }
}

void BandwidthSlice::copied(std::size_t dstOffset, std::size_t srcOffset, std::size_t length)
{
    for (std::size_t k = 0; k < length / kBwCell; ++k) {
        m_cellBase[dstOffset / kBwCell + k] = m_cellBase[srcOffset / kBwCell + k];
        m_known[dstOffset / kBwCell + k] = m_known[srcOffset / kBwCell + k];
    }
}

std::size_t BandwidthSlice::nextCopyShift(std::size_t dstOffset)
{
    const std::size_t cells = m_bytes / 2 / kBwCell;
    std::uint32_t &shift = m_copyShift[(dstOffset - m_bytes / 2) / kBwCell];
    shift = std::uint32_t((shift + 1) % cells);
    return shift;
}

// --- BandwidthKernel ---

namespace {

class BandwidthThread : public cpu::KernelThread
{
public:
    BandwidthThread(const BwVariant &variant, const BwFunctions &fn, std::vector<BandwidthSlice *> slices,
                    std::size_t block)
        : m_op(variant.op), m_nt(variant.stores == BwStores::NonTemporal), m_fn(fn), m_slices(std::move(slices)),
          m_block(block), m_perSlice(m_slices.front()->bytes() / block)
    {
    }

    std::uint64_t unit(std::uint64_t unitIndex) override
    {
        BandwidthSlice &s = *m_slices[m_slice];
        std::uint64_t result = 0;
        switch (m_op) {
        case BwOp::Read: {
            const std::size_t off = m_at * m_block;
            result = m_fn.read(cpu::opaqueValue(static_cast<const void *>(s.data() + off)), m_block);
            if (result != s.expectedSum(off, m_block))
                throw std::runtime_error("the kernel produced a wrong sum: possible miscompile or memory fault");
            break;
        }
        case BwOp::Write: {
            const std::size_t off = m_at * m_block;
            const std::uint64_t base = mix(unitIndex, 0x57A7E);
            (m_nt ? m_fn.writeNt : m_fn.write)(cpu::opaqueValue(static_cast<void *>(s.data() + off)), m_block,
                                               cpu::opaqueValue(base));
            s.wrote(off, m_block, base);
            if (!s.verifySample(off, m_block, unitIndex))
                throw std::runtime_error("a sampled line doesn't hold what was stored: possible miscompile or memory "
                                         "fault");
            result = base;
            break;
        }
        case BwOp::Copy: {
            // B / 2 into the second half, from the first half rotated by
            // nextCopyShift() cells (see the header): one or two pieces.
            const std::size_t chunk = m_block / 2;
            const std::size_t sliceHalf = s.bytes() / 2;
            const std::size_t dst = sliceHalf + m_at * chunk;
            const std::size_t src = (m_at * chunk + s.nextCopyShift(dst) * kBwCell) % sliceHalf;
            const std::size_t first = std::min(chunk, sliceHalf - src);
            const auto copy = m_nt ? m_fn.copyNt : m_fn.copy;
            copy(cpu::opaqueValue(static_cast<void *>(s.data() + dst)),
                 cpu::opaqueValue(static_cast<const void *>(s.data() + src)), first);
            if (first < chunk)
                copy(cpu::opaqueValue(static_cast<void *>(s.data() + dst + first)),
                     cpu::opaqueValue(static_cast<const void *>(s.data())), chunk - first);
            if (!s.verifyCopySample(dst, src, first, unitIndex)
                || (first < chunk && !s.verifyCopySample(dst + first, 0, chunk - first, unitIndex)))
                throw std::runtime_error("a sampled line of the copy doesn't match its source: possible miscompile or "
                                         "memory fault");
            s.copied(dst, src, first);
            if (first < chunk)
                s.copied(dst + first, 0, chunk - first);
            result = s.expectedSum(dst, chunk);
            break;
        }
        }
        if (++m_at == m_perSlice) {
            m_at = 0;
            m_slice = (m_slice + 1) % m_slices.size();
        }
        return result;
    }

private:
    BwOp m_op;
    bool m_nt;
    BwFunctions m_fn;
    std::vector<BandwidthSlice *> m_slices;
    std::size_t m_block;
    std::size_t m_perSlice;  // units per slice (copy: of B / 2 each, over the slice's halves)
    std::size_t m_slice = 0;
    std::size_t m_at = 0;
};

} // namespace

BandwidthKernel::BandwidthKernel(BwPath path, BwVariant variant, std::vector<BandwidthSlice *> slices, int threads)
    : BandwidthKernel(bandwidthFunctions(path), std::move(variant), std::move(slices), threads)
{
}

BandwidthKernel::BandwidthKernel(const BwFunctions &functions, BwVariant variant, std::vector<BandwidthSlice *> slices,
                                 int threads)
    : m_variant(std::move(variant)), m_fn(functions), m_slices(std::move(slices)), m_threads(threads),
      m_name(opName(m_variant))
{
    if (m_slices.empty() || (threads != 1 && std::size_t(threads) != m_slices.size()))
        throw std::invalid_argument("BandwidthKernel: one thread over every slice, or one slice per thread");
    const std::size_t size = m_slices.front()->bytes();
    for (const BandwidthSlice *s : m_slices) {
        if (s->bytes() != size)
            throw std::invalid_argument("BandwidthKernel: the slices must be the same size");
    }
    m_block = std::min(kBwBlock, size);
    if (size % m_block != 0)
        throw std::invalid_argument("BandwidthKernel: a slice must be whole blocks (planBandwidth)");
    const bool nt = m_variant.stores == BwStores::NonTemporal;
    const bool have = m_variant.op == BwOp::Read    ? m_fn.read != nullptr
                      : m_variant.op == BwOp::Write ? (nt ? m_fn.writeNt : m_fn.write) != nullptr
                                                    : (nt ? m_fn.copyNt : m_fn.copy) != nullptr;
    if (!have)
        throw std::invalid_argument("BandwidthKernel: this path has no " + m_name);
}

std::unique_ptr<cpu::KernelThread> BandwidthKernel::makeThread(int thread) const
{
    std::vector<BandwidthSlice *> mine;
    if (m_threads == 1)
        mine = m_slices;
    else
        mine = {m_slices[std::size_t(thread)]};
    return std::make_unique<BandwidthThread>(m_variant, m_fn, std::move(mine), m_block);
}

// --- Sizes ---

std::string BandwidthPlan::describe() const
{
    // Whole GiB, else MiB (exact: no rounding that could differ between C
    // libraries).
    const std::uint64_t t = totalBytes();
    const std::string total = t % GiB == 0 ? std::to_string(t / GiB) + " GiB" : std::to_string(t / MiB) + " MiB";
    return total + " in total (" + std::to_string(sliceBytes / MiB) + " MiB per thread)";
}

BandwidthPlan planBandwidth(std::uint64_t totalLastLevelBytes, int threads)
{
    constexpr std::uint64_t kMin = 1 * GiB, kMax = 4 * GiB;
    BandwidthPlan p;
    p.threads = std::max(1, threads);
    const std::uint64_t want = std::min(kMax, std::max(kMin, 8 * totalLastLevelBytes));
    const auto n = std::uint64_t(p.threads);
    std::size_t slice = roundUp(std::size_t((want + n - 1) / n), kBwSliceUnit);
    std::size_t unit = kBwSliceUnit;
    if (slice > kBwBlock) {
        slice = roundUp(slice, kBwBlock);
        unit = kBwBlock;
    }
    if (std::uint64_t(slice) * n > kMax)
        slice = std::max(unit, std::size_t(kMax / n) / unit * unit);
    p.sliceBytes = slice;
    p.blockBytes = std::min(kBwBlock, slice);
    return p;
}

std::uint64_t totalLastLevelCache(const std::vector<isa::CacheTopology> &perCpu)
{
    std::set<std::vector<int>> seen;
    std::uint64_t total = 0;
    for (const isa::CacheTopology &t : perCpu) {
        const isa::CacheLevel *last = t.lastLevel();
        if (!last)
            continue;
        // A cache that names no sharers is counted once per CPU reporting it.
        if (last->sharedWith.empty() || seen.insert(last->sharedWith).second)
            total += last->sizeBytes;
    }
    return total;
}

} // namespace sb::ram
