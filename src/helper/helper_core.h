// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The root helper's request handling (§6), separate from main() so it can be
// tested in-process without root. No Qt: libc/libstdc++ and the protocol
// codec only.

#include "privileged/protocol/protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sb::helper {

struct Config {
    // Fixed in the shipped helper; only the test binary sets other paths.
    std::string entryPointPath = "/sys/firmware/dmi/tables/smbios_entry_point";
    std::string tablePath = "/sys/firmware/dmi/tables/DMI";
    std::string buildId;
    std::uint32_t euid = 0;
};

class Core
{
public:
    static constexpr int kMaxBadFramesInARow = 3;
    static constexpr int kExitShutdown = 0;
    static constexpr int kExitTooManyBadFrames = 3;

    explicit Core(Config config) : m_config(std::move(config)) {}

    // Sent unprompted when the helper starts.
    proto::Bytes hello() const;

    struct Output {
        std::vector<proto::Bytes> replies;  // to write to stdout, in order
        bool exit = false;
        int exitCode = 0;
    };
    // Feeds bytes read from stdin.
    Output feed(const std::uint8_t *data, std::size_t size);

private:
    proto::Frame handle(const proto::Frame &request, bool &bad, bool &shutdown) const;

    Config m_config;
    proto::Decoder m_decoder{proto::kMaxRequestFrame};
    int m_badInARow = 0;
};

// Writes HELLO, then answers requests from `inFd` on `outFd` until EOF
// (returns 0), SHUTDOWN, too many bad frames, or an I/O error (returns 1).
int serve(Core &core, int inFd, int outFd);

// Reads a whole file (no symlinks followed) up to `cap` bytes. Returns 0 or
// an errno value (EFBIG if it's larger than `cap`).
int readWholeFile(const std::string &path, std::size_t cap, proto::Bytes &out);

} // namespace sb::helper
