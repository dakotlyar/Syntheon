// SPDX-License-Identifier: GPL-3.0-or-later
// Engine selection (Phase 4b design §4.1, D7). Depth 1 is always the sync
// engine (sequential by definition); the queued depths pick the platform's
// async backend at the given queue depth: overlapped on Windows, io_uring on
// Linux x64 when liburing was compiled, else libaio on Linux, else sync.
#include "tests/storage/io/engine.h"

namespace sb::storage {

std::unique_ptr<IStorageIoEngine> firstUsableEngine(std::vector<std::unique_ptr<IStorageIoEngine>> candidates)
{
    for (auto &c : candidates)
        if (c && c->usable())
            return std::move(c);
    return makeSyncEngine();
}

std::unique_ptr<IStorageIoEngine> pickEngine(IoDepth depth, std::size_t queueDepth)
{
    if (depth == IoDepth::One)
        return makeSyncEngine();
#if defined(_WIN32)
    return makeWinEngine(queueDepth);
#else
    // Compile-time availability is not enough: io_uring can be refused at run
    // time (seccomp, kernel.io_uring_disabled), libaio likewise. Probe each in
    // preference order and fall back, ending at sync.
    std::vector<std::unique_ptr<IStorageIoEngine>> candidates;
#if defined(SYNTHEON_HAVE_LIBURING)
    candidates.push_back(makeUringEngine(queueDepth));
#endif
#if defined(SYNTHEON_HAVE_LIBAIO)
    candidates.push_back(makeAioEngine(queueDepth));
#endif
    return firstUsableEngine(std::move(candidates));
#endif
}

} // namespace sb::storage
