// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The four codecs of the compression sub-test (Phase 2 design §5.4, P11), at
// the design's levels: zlib 6, zstd 3, LZ4 default (LZ4_compress_default,
// acceleration 1), xz preset 6 with the dictionary capped at the 256 KiB
// block size (a few MiB of encoder state per thread instead of 94 MiB). zstd
// and liblzma are built without their own threading (cmake/ThirdParty.cmake);
// SynBench does its own.
//
// One CodecContext per thread and codec, reused for every block (deflateReset,
// one ZSTD_CCtx / ZSTD_DCtx, one lzma_stream re-initialised in place).
// Compression of zlib, zstd and xz is streamed in 64 KiB pieces of input,
// with the cancel flag checked between pieces, so even xz on a slow CPU
// returns from a Stop within the 250 ms contract; LZ4 (one call, well under
// a millisecond per block) and every decompression (one call: a few ms at
// most) aren't. The piece size is part of what is measured and of the golden
// sizes: it is fixed.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace sb::cpu {

enum class Codec { Zlib, Zstd, Lz4, Xz };
inline constexpr Codec kCodecs[] = {Codec::Zlib, Codec::Zstd, Codec::Lz4, Codec::Xz};
inline constexpr std::size_t kCodecCount = 4;
inline constexpr std::size_t kStreamPiece = 64 * 1024;

// "zlib", "zstd", "LZ4", "xz" (for messages and labels).
const char *codecName(Codec c);
// "zlib", "zstd", "lz4", "xz" (for metric keys: compress.<key>.c).
const char *codecKey(Codec c);
// "zlib (level 6)", "zstd (level 3)", "LZ4 (default)", "xz (preset 6)".
const char *codecLabel(Codec c);

// Whether the codec libraries were compiled optimised: not in a Debug build
// (they get the build type's flags; Dave, 2026-10-17: Debug numbers are
// local-only and flagged, nothing is forced).
bool codecsOptimised();

class CodecContext
{
public:
    explicit CodecContext(Codec codec);
    ~CodecContext();
    CodecContext(const CodecContext &) = delete;
    CodecContext &operator=(const CodecContext &) = delete;

    Codec codec() const { return m_codec; }
    // The most `n` bytes can compress to.
    static std::size_t bound(Codec codec, std::size_t n);

    // Compresses `in` into `out` (at least bound(in.size()) bytes); the
    // compressed size, or nothing if `cancel` was set (checked between 64 KiB
    // pieces). Throws std::runtime_error if the library fails.
    std::optional<std::size_t> compress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out,
                                        const std::atomic<bool> *cancel = nullptr);
    // Decompresses `in`, which must decompress to exactly out.size() bytes;
    // throws std::runtime_error on a corrupt stream or a size mismatch.
    void decompress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out);

private:
    struct Impl;
    Codec m_codec;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sb::cpu
