// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GUI <-> synbench-helper wire protocol (§6). Plain C++20, no Qt: it is
// compiled into the root helper, which links nothing but libc/libstdc++.
//
// Frame (all integers little-endian):
//   u32 length      bytes that follow this field: 8 + payload size
//   u32 request_id  echoed in the response; 0 for the helper's HELLO
//   u16 op          Op
//   u16 flags       low byte: Flag bits; high byte: protocol version
//   payload
// Requests are capped at 4 KiB, responses at 4 MiB (whole frame).
//
// Payloads:
//   HELLO (helper -> GUI, unprompted, Response flag):
//       u32 protocol_version, u32 euid, u16 n, n bytes build id
//   PING, SHUTDOWN: empty request, empty response
//   READ_SMBIOS: empty request; response u32 n, n bytes smbios_entry_point,
//       u32 m, m bytes DMI table
//   Error response (Response|Error flags): u16 ErrorCode, u16 n, n bytes
//       message (UTF-8)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sb::proto {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 12;  // length + request_id + op + flags
inline constexpr std::uint32_t kMaxRequestFrame = 4 * 1024;
inline constexpr std::uint32_t kMaxResponseFrame = 4 * 1024 * 1024;

enum class Op : std::uint16_t {
    Hello = 1,
    Ping = 2,
    ReadSmbios = 3,
    Shutdown = 4,
};

enum Flag : std::uint8_t {
    Response = 0x01,
    Error = 0x02,
};

enum class ErrorCode : std::uint16_t {
    UnknownOp = 1,
    Oversized = 2,
    Malformed = 3,
    BadVersion = 4,
    NotPermitted = 5,  // EACCES/EPERM: e.g. the helper isn't root
    IoError = 6,
};

struct Frame {
    std::uint32_t requestId = 0;
    std::uint16_t op = 0;
    std::uint8_t flags = 0;
    std::uint8_t version = kProtocolVersion;
    Bytes payload;

    bool isResponse() const { return flags & Response; }
    bool isError() const { return flags & Error; }
};

Bytes encode(const Frame &frame);

// Incremental decoder over a byte stream (a pipe). A frame larger than the
// limit is reported once and then skipped byte by byte without buffering,
// so framing stays in sync.
class Decoder
{
public:
    explicit Decoder(std::uint32_t maxFrame) : m_maxFrame(maxFrame) {}

    void feed(const std::uint8_t *data, std::size_t size);

    enum class Status { NeedMore, Frame, Oversized };
    // Frame: `out` holds the next frame. Oversized: `out.requestId`/`op`
    // hold what the oversized frame claimed, if its header had arrived.
    Status next(Frame &out);

    std::size_t buffered() const { return m_buffer.size(); }

private:
    std::uint32_t m_maxFrame;
    Bytes m_buffer;
    std::uint64_t m_skip = 0;  // bytes of an oversized frame still to drop
};

// Payload codecs. Decoders return nullopt on malformed input (wrong size,
// trailing bytes, lengths past the end).
struct Hello {
    std::uint32_t protocolVersion = kProtocolVersion;
    std::uint32_t euid = 0;
    std::string buildId;
};
Bytes encodeHello(const Hello &hello);
std::optional<Hello> decodeHello(const Bytes &payload);

struct SmbiosBlobs {
    Bytes entryPoint;
    Bytes table;
};
Bytes encodeSmbios(const SmbiosBlobs &blobs);
std::optional<SmbiosBlobs> decodeSmbios(const Bytes &payload);

struct ErrorReply {
    ErrorCode code = ErrorCode::Malformed;
    std::string message;
};
Bytes encodeError(const ErrorReply &error);
std::optional<ErrorReply> decodeError(const Bytes &payload);

// Response frame helpers.
Frame makeResponse(const Frame &request, Bytes payload = {});
Frame makeError(std::uint32_t requestId, std::uint16_t op, ErrorCode code, const std::string &message);

} // namespace sb::proto
