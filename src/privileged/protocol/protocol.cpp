// SPDX-License-Identifier: GPL-3.0-or-later
#include "privileged/protocol/protocol.h"

#include <algorithm>

namespace sb::proto {

namespace {

void put16(Bytes &b, std::uint16_t v)
{
    b.push_back(std::uint8_t(v));
    b.push_back(std::uint8_t(v >> 8));
}

void put32(Bytes &b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(std::uint8_t(v >> (8 * i)));
}

std::uint16_t get16(const std::uint8_t *p)
{
    return std::uint16_t(p[0] | (p[1] << 8));
}

std::uint32_t get32(const std::uint8_t *p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

// Bounds-checked reader over a payload.
class Reader
{
public:
    explicit Reader(const Bytes &b) : m_b(b) {}
    bool u16(std::uint16_t &v)
    {
        if (left() < 2)
            return false;
        v = get16(m_b.data() + m_pos);
        m_pos += 2;
        return true;
    }
    bool u32(std::uint32_t &v)
    {
        if (left() < 4)
            return false;
        v = get32(m_b.data() + m_pos);
        m_pos += 4;
        return true;
    }
    bool bytes(std::size_t n, Bytes &out)
    {
        if (left() < n)
            return false;
        out.assign(m_b.begin() + std::ptrdiff_t(m_pos), m_b.begin() + std::ptrdiff_t(m_pos + n));
        m_pos += n;
        return true;
    }
    bool atEnd() const { return m_pos == m_b.size(); }

private:
    std::size_t left() const { return m_b.size() - m_pos; }
    const Bytes &m_b;
    std::size_t m_pos = 0;
};

} // namespace

Bytes encode(const Frame &f)
{
    Bytes b;
    b.reserve(kHeaderSize + f.payload.size());
    put32(b, std::uint32_t(8 + f.payload.size()));
    put32(b, f.requestId);
    put16(b, f.op);
    put16(b, std::uint16_t(f.flags | (f.version << 8)));
    b.insert(b.end(), f.payload.begin(), f.payload.end());
    return b;
}

void Decoder::feed(const std::uint8_t *data, std::size_t size)
{
    // Drop what's left of an oversized frame first.
    const std::size_t skip = std::size_t(std::min<std::uint64_t>(m_skip, size));
    m_skip -= skip;
    m_buffer.insert(m_buffer.end(), data + skip, data + size);
}

Decoder::Status Decoder::next(Frame &out)
{
    if (m_skip > 0 || m_buffer.size() < 4)
        return Status::NeedMore;
    const std::uint32_t length = get32(m_buffer.data());
    const std::uint64_t total = 4 + std::uint64_t(length);
    if (length < 8 || total > m_maxFrame) {
        // Oversized (or a length too small to hold a header): report it and
        // drop the claimed frame.
        out = Frame{};
        if (m_buffer.size() >= kHeaderSize) {
            out.requestId = get32(m_buffer.data() + 4);
            out.op = get16(m_buffer.data() + 8);
        }
        const std::uint64_t dropNow = std::min<std::uint64_t>(total, m_buffer.size());
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + std::ptrdiff_t(dropNow));
        m_skip = total - dropNow;
        return Status::Oversized;
    }
    if (m_buffer.size() < total)
        return Status::NeedMore;
    out.requestId = get32(m_buffer.data() + 4);
    out.op = get16(m_buffer.data() + 8);
    const std::uint16_t flags = get16(m_buffer.data() + 10);
    out.flags = std::uint8_t(flags & 0xff);
    out.version = std::uint8_t(flags >> 8);
    out.payload.assign(m_buffer.begin() + std::ptrdiff_t(kHeaderSize), m_buffer.begin() + std::ptrdiff_t(total));
    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + std::ptrdiff_t(total));
    return Status::Frame;
}

Bytes encodeHello(const Hello &h)
{
    Bytes b;
    put32(b, h.protocolVersion);
    put32(b, h.euid);
    const std::size_t n = std::min<std::size_t>(h.buildId.size(), 0xffff);
    put16(b, std::uint16_t(n));
    b.insert(b.end(), h.buildId.begin(), h.buildId.begin() + std::ptrdiff_t(n));
    return b;
}

std::optional<Hello> decodeHello(const Bytes &payload)
{
    Reader r(payload);
    Hello h;
    std::uint16_t n = 0;
    Bytes id;
    if (!r.u32(h.protocolVersion) || !r.u32(h.euid) || !r.u16(n) || !r.bytes(n, id) || !r.atEnd())
        return std::nullopt;
    h.buildId.assign(id.begin(), id.end());
    return h;
}

Bytes encodeSmbios(const SmbiosBlobs &s)
{
    Bytes b;
    put32(b, std::uint32_t(s.entryPoint.size()));
    b.insert(b.end(), s.entryPoint.begin(), s.entryPoint.end());
    put32(b, std::uint32_t(s.table.size()));
    b.insert(b.end(), s.table.begin(), s.table.end());
    return b;
}

std::optional<SmbiosBlobs> decodeSmbios(const Bytes &payload)
{
    Reader r(payload);
    SmbiosBlobs s;
    std::uint32_t n = 0, m = 0;
    if (!r.u32(n) || !r.bytes(n, s.entryPoint) || !r.u32(m) || !r.bytes(m, s.table) || !r.atEnd())
        return std::nullopt;
    return s;
}

Bytes encodeError(const ErrorReply &e)
{
    Bytes b;
    put16(b, std::uint16_t(e.code));
    const std::size_t n = std::min<std::size_t>(e.message.size(), 1024);
    put16(b, std::uint16_t(n));
    b.insert(b.end(), e.message.begin(), e.message.begin() + std::ptrdiff_t(n));
    return b;
}

std::optional<ErrorReply> decodeError(const Bytes &payload)
{
    Reader r(payload);
    std::uint16_t code = 0, n = 0;
    Bytes msg;
    if (!r.u16(code) || !r.u16(n) || !r.bytes(n, msg) || !r.atEnd())
        return std::nullopt;
    return ErrorReply{ErrorCode(code), std::string(msg.begin(), msg.end())};
}

Frame makeResponse(const Frame &request, Bytes payload)
{
    Frame f;
    f.requestId = request.requestId;
    f.op = request.op;
    f.flags = Response;
    f.payload = std::move(payload);
    return f;
}

Frame makeError(std::uint32_t requestId, std::uint16_t op, ErrorCode code, const std::string &message)
{
    Frame f;
    f.requestId = requestId;
    f.op = op;
    f.flags = Response | Error;
    f.payload = encodeError({code, message});
    return f;
}

} // namespace sb::proto
