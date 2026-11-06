// SPDX-License-Identifier: GPL-3.0-or-later
#include "helper/helper_core.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace sb::helper {

using namespace sb::proto;

int readWholeFile(const std::string &path, std::size_t cap, Bytes &out)
{
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    if (fd < 0)
        return errno;
    std::uint8_t buf[16384];
    int result = 0;
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            result = errno;
            break;
        }
        if (n == 0)
            break;
        if (out.size() + std::size_t(n) > cap) {
            result = EFBIG;
            break;
        }
        out.insert(out.end(), buf, buf + n);
    }
    ::close(fd);
    if (result != 0)
        out.clear();
    return result;
}

namespace {

bool writeAll(int fd, const Bytes &bytes)
{
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;  // EPIPE: Syntheon is gone
        }
        done += std::size_t(n);
    }
    return true;
}

} // namespace

int serve(Core &core, int inFd, int outFd)
{
    if (!writeAll(outFd, core.hello()))
        return 1;
    std::uint8_t buf[8192];
    for (;;) {
        const ssize_t n = ::read(inFd, buf, sizeof buf);
        if (n == 0)
            return 0;  // EOF: Syntheon exited or crashed. Nothing to restore in Phase 1.
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        const auto out = core.feed(buf, std::size_t(n));
        for (const auto &reply : out.replies) {
            if (!writeAll(outFd, reply))
                return 1;
        }
        if (out.exit)
            return out.exitCode;
    }
}

Bytes Core::hello() const
{
    Frame f;
    f.requestId = 0;
    f.op = std::uint16_t(Op::Hello);
    f.flags = Response;
    f.payload = encodeHello({kProtocolVersion, m_config.euid, m_config.buildId});
    return encode(f);
}

Frame Core::handle(const Frame &req, bool &bad, bool &shutdown) const
{
    bad = true;
    if (req.version != kProtocolVersion)
        return makeError(req.requestId, req.op, ErrorCode::BadVersion,
                         "protocol version " + std::to_string(req.version) + ", expected " +
                             std::to_string(kProtocolVersion));
    if (req.flags != 0)
        return makeError(req.requestId, req.op, ErrorCode::Malformed, "requests carry no flags");

    switch (Op(req.op)) {
    case Op::Ping:
    case Op::ReadSmbios:
    case Op::Shutdown:
        if (!req.payload.empty())
            return makeError(req.requestId, req.op, ErrorCode::Malformed, "this request takes no payload");
        break;
    default:
        return makeError(req.requestId, req.op, ErrorCode::UnknownOp, "unknown op " + std::to_string(req.op));
    }

    bad = false;
    switch (Op(req.op)) {
    case Op::Ping:
        return makeResponse(req);
    case Op::Shutdown:
        shutdown = true;
        return makeResponse(req);
    case Op::ReadSmbios: {
        // Fixed paths. The bytes go back raw: parsing happens in the
        // unprivileged GUI, never here.
        SmbiosBlobs blobs;
        const std::size_t tableCap = kMaxResponseFrame - kHeaderSize - 8 - 64;
        int err = readWholeFile(m_config.entryPointPath, 64, blobs.entryPoint);
        if (err == 0)
            err = readWholeFile(m_config.tablePath, tableCap, blobs.table);
        if (err == EACCES || err == EPERM)
            return makeError(req.requestId, req.op, ErrorCode::NotPermitted, std::strerror(err));
        if (err != 0)
            return makeError(req.requestId, req.op, ErrorCode::IoError, std::strerror(err));
        return makeResponse(req, encodeSmbios(blobs));
    }
    default:
        break;
    }
    bad = true;  // unreachable: every allowed op is handled above
    return makeError(req.requestId, req.op, ErrorCode::UnknownOp, "unknown op");
}

Core::Output Core::feed(const std::uint8_t *data, std::size_t size)
{
    Output out;
    m_decoder.feed(data, size);
    Frame req;
    for (;;) {
        const Decoder::Status status = m_decoder.next(req);
        if (status == Decoder::Status::NeedMore)
            break;
        bool bad = true, shutdown = false;
        Frame reply;
        if (status == Decoder::Status::Oversized)
            reply = makeError(req.requestId, req.op, ErrorCode::Oversized, "request frames are capped at 4 KiB");
        else
            reply = handle(req, bad, shutdown);
        out.replies.push_back(encode(reply));

        m_badInARow = bad ? m_badInARow + 1 : 0;
        if (m_badInARow >= kMaxBadFramesInARow) {
            out.exit = true;
            out.exitCode = kExitTooManyBadFrames;
            return out;
        }
        if (shutdown) {
            out.exit = true;
            out.exitCode = kExitShutdown;
            return out;
        }
    }
    return out;
}

} // namespace sb::helper
