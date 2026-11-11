// SPDX-License-Identifier: GPL-3.0-or-later
// Engine selection (Phase 4b design §4.1, D7). Depth 1 is always the sync
// engine (sequential by definition); the queued depths pick the platform's
// async backend at the given queue depth: overlapped on Windows, io_uring on
// Linux x64 when liburing was compiled, else libaio on Linux, else sync.
#include "tests/storage/io/engine.h"

namespace sb::storage {

std::unique_ptr<IStorageIoEngine> pickEngine(IoDepth depth, std::size_t queueDepth)
{
    if (depth == IoDepth::One)
        return makeSyncEngine();
#if defined(_WIN32)
    return makeWinEngine(queueDepth);
#elif defined(SYNTHEON_HAVE_LIBURING)
    return makeUringEngine(queueDepth);
#elif defined(SYNTHEON_HAVE_LIBAIO)
    return makeAioEngine(queueDepth);
#else
    return makeSyncEngine();
#endif
}

} // namespace sb::storage
