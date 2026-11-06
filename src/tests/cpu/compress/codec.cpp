// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/compress/codec.h"

#include <lz4.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <climits>
#include <stdexcept>
#include <string>

namespace sb::cpu {

namespace {

constexpr int kZlibLevel = 6;
constexpr int kZstdLevel = 3;
constexpr std::uint32_t kXzPreset = 6;
constexpr std::uint32_t kXzDictSize = 256 * 1024;  // the block size (§5.4)

[[noreturn]] void fail(Codec c, const std::string &what)
{
    throw std::runtime_error(std::string(codecName(c)) + ": " + what);
}

bool cancelled(const std::atomic<bool> *cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}

} // namespace

const char *codecName(Codec c)
{
    switch (c) {
    case Codec::Zlib: return "zlib";
    case Codec::Zstd: return "zstd";
    case Codec::Lz4: return "LZ4";
    case Codec::Xz: return "xz";
    }
    return "?";
}

const char *codecKey(Codec c)
{
    switch (c) {
    case Codec::Zlib: return "zlib";
    case Codec::Zstd: return "zstd";
    case Codec::Lz4: return "lz4";
    case Codec::Xz: return "xz";
    }
    return "?";
}

const char *codecLabel(Codec c)
{
    switch (c) {
    case Codec::Zlib: return "zlib (level 6)";
    case Codec::Zstd: return "zstd (level 3)";
    case Codec::Lz4: return "LZ4 (default)";
    case Codec::Xz: return "xz (preset 6)";
    }
    return "?";
}

bool codecsOptimised()
{
    return SYNTHEON_THIRD_PARTY_OPTIMISED != 0;
}

struct CodecContext::Impl {
    z_stream deflater{};
    z_stream inflater{};
    bool deflaterReady = false, inflaterReady = false;
    ZSTD_CCtx *zc = nullptr;
    ZSTD_DCtx *zd = nullptr;
    lzma_stream xzEnc = LZMA_STREAM_INIT;
    lzma_stream xzDec = LZMA_STREAM_INIT;
    lzma_options_lzma xzOptions{};
    lzma_filter xzFilters[2]{};
};

CodecContext::CodecContext(Codec codec) : m_codec(codec), m_impl(std::make_unique<Impl>())
{
    Impl &d = *m_impl;
    switch (codec) {
    case Codec::Zlib:
        if (deflateInit(&d.deflater, kZlibLevel) != Z_OK)
            fail(codec, "deflateInit failed");
        d.deflaterReady = true;
        if (inflateInit(&d.inflater) != Z_OK)
            fail(codec, "inflateInit failed");
        d.inflaterReady = true;
        break;
    case Codec::Zstd:
        d.zc = ZSTD_createCCtx();
        d.zd = ZSTD_createDCtx();
        if (!d.zc || !d.zd || ZSTD_isError(ZSTD_CCtx_setParameter(d.zc, ZSTD_c_compressionLevel, kZstdLevel)))
            fail(codec, "context setup failed");
        break;
    case Codec::Lz4:
        break;
    case Codec::Xz:
        if (lzma_lzma_preset(&d.xzOptions, kXzPreset))
            fail(codec, "preset 6 unavailable");
        d.xzOptions.dict_size = kXzDictSize;
        d.xzFilters[0] = {LZMA_FILTER_LZMA2, &d.xzOptions};
        d.xzFilters[1] = {LZMA_VLI_UNKNOWN, nullptr};
        break;
    }
}

CodecContext::~CodecContext()
{
    Impl &d = *m_impl;
    if (d.deflaterReady)
        deflateEnd(&d.deflater);
    if (d.inflaterReady)
        inflateEnd(&d.inflater);
    ZSTD_freeCCtx(d.zc);
    ZSTD_freeDCtx(d.zd);
    lzma_end(&d.xzEnc);
    lzma_end(&d.xzDec);
}

std::size_t CodecContext::bound(Codec codec, std::size_t n)
{
    switch (codec) {
    case Codec::Zlib: return std::size_t(compressBound(uLong(n)));
    case Codec::Zstd: return ZSTD_compressBound(n);
    case Codec::Lz4: return std::size_t(LZ4_compressBound(int(n)));
    case Codec::Xz: return lzma_stream_buffer_bound(n);
    }
    return 0;
}

std::optional<std::size_t> CodecContext::compress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out,
                                                  const std::atomic<bool> *cancel)
{
    Impl &d = *m_impl;
    if (in.size() > std::size_t(INT_MAX) || out.size() < bound(m_codec, in.size()))
        fail(m_codec, "compress buffers out of range");
    switch (m_codec) {
    case Codec::Zlib: {
        z_stream &z = d.deflater;
        if (deflateReset(&z) != Z_OK)
            fail(m_codec, "deflateReset failed");
        z.next_out = out.data();
        z.avail_out = uInt(out.size());
        for (std::size_t at = 0; at < in.size() || in.empty(); at += kStreamPiece) {
            const std::size_t n = std::min(kStreamPiece, in.size() - at);
            const bool last = at + n >= in.size();
            z.next_in = const_cast<Bytef *>(in.data() + at);
            z.avail_in = uInt(n);
            const int r = deflate(&z, last ? Z_FINISH : Z_NO_FLUSH);
            if (last) {
                if (r != Z_STREAM_END)
                    fail(m_codec, "deflate didn't finish");
                return std::size_t(z.total_out);
            }
            if (r != Z_OK || z.avail_in != 0)
                fail(m_codec, "deflate failed");
            if (cancelled(cancel))
                return std::nullopt;
        }
        return std::nullopt;  // unreachable
    }
    case Codec::Zstd: {
        if (ZSTD_isError(ZSTD_CCtx_reset(d.zc, ZSTD_reset_session_only))
            || ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(d.zc, in.size())))
            fail(m_codec, "context reset failed");
        ZSTD_outBuffer ob{out.data(), out.size(), 0};
        for (std::size_t at = 0; at < in.size() || in.empty(); at += kStreamPiece) {
            const std::size_t n = std::min(kStreamPiece, in.size() - at);
            const bool last = at + n >= in.size();
            ZSTD_inBuffer ib{in.data() + at, n, 0};
            for (;;) {
                const std::size_t r = ZSTD_compressStream2(d.zc, &ob, &ib, last ? ZSTD_e_end : ZSTD_e_continue);
                if (ZSTD_isError(r))
                    fail(m_codec, ZSTD_getErrorName(r));
                if (last ? r == 0 : ib.pos == ib.size)
                    break;
            }
            if (last)
                return ob.pos;
            if (cancelled(cancel))
                return std::nullopt;
        }
        return std::nullopt;  // unreachable
    }
    case Codec::Lz4: {
        const int r = LZ4_compress_default(reinterpret_cast<const char *>(in.data()), reinterpret_cast<char *>(out.data()),
                                           int(in.size()), int(std::min<std::size_t>(out.size(), INT_MAX)));
        if (r <= 0)
            fail(m_codec, "LZ4_compress_default failed");
        return std::size_t(r);
    }
    case Codec::Xz: {
        lzma_stream &s = d.xzEnc;
        // Re-initialising the same stream keeps its memory when the filters
        // are the same.
        if (lzma_stream_encoder(&s, d.xzFilters, LZMA_CHECK_CRC64) != LZMA_OK)
            fail(m_codec, "lzma_stream_encoder failed");
        s.next_out = out.data();
        s.avail_out = out.size();
        for (std::size_t at = 0; at < in.size() || in.empty(); at += kStreamPiece) {
            const std::size_t n = std::min(kStreamPiece, in.size() - at);
            const bool last = at + n >= in.size();
            s.next_in = in.data() + at;
            s.avail_in = n;
            for (;;) {
                const lzma_ret r = lzma_code(&s, last ? LZMA_FINISH : LZMA_RUN);
                if (last && r == LZMA_STREAM_END)
                    return std::size_t(s.total_out);
                if (r != LZMA_OK)
                    fail(m_codec, "lzma_code failed (" + std::to_string(int(r)) + ")");
                if (!last && s.avail_in == 0)
                    break;
            }
            if (cancelled(cancel))
                return std::nullopt;
        }
        return std::nullopt;  // unreachable
    }
    }
    return std::nullopt;
}

void CodecContext::decompress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out)
{
    Impl &d = *m_impl;
    switch (m_codec) {
    case Codec::Zlib: {
        z_stream &z = d.inflater;
        if (inflateReset(&z) != Z_OK)
            fail(m_codec, "inflateReset failed");
        z.next_in = const_cast<Bytef *>(in.data());
        z.avail_in = uInt(in.size());
        z.next_out = out.data();
        z.avail_out = uInt(out.size());
        if (inflate(&z, Z_FINISH) != Z_STREAM_END || z.total_out != out.size())
            fail(m_codec, "corrupt stream");
        return;
    }
    case Codec::Zstd: {
        const std::size_t r = ZSTD_decompressDCtx(d.zd, out.data(), out.size(), in.data(), in.size());
        if (ZSTD_isError(r) || r != out.size())
            fail(m_codec, ZSTD_isError(r) ? ZSTD_getErrorName(r) : "wrong decompressed size");
        return;
    }
    case Codec::Lz4: {
        const int r = LZ4_decompress_safe(reinterpret_cast<const char *>(in.data()), reinterpret_cast<char *>(out.data()),
                                          int(in.size()), int(out.size()));
        if (r < 0 || std::size_t(r) != out.size())
            fail(m_codec, "corrupt stream");
        return;
    }
    case Codec::Xz: {
        lzma_stream &s = d.xzDec;
        if (lzma_stream_decoder(&s, UINT64_MAX, 0) != LZMA_OK)
            fail(m_codec, "lzma_stream_decoder failed");
        s.next_in = in.data();
        s.avail_in = in.size();
        s.next_out = out.data();
        s.avail_out = out.size();
        lzma_ret r;
        do
            r = lzma_code(&s, LZMA_FINISH);
        while (r == LZMA_OK);  // a call without progress ends with LZMA_BUF_ERROR
        if (r != LZMA_STREAM_END || s.total_out != out.size())
            fail(m_codec, "corrupt stream");
        return;
    }
    }
}

} // namespace sb::cpu
