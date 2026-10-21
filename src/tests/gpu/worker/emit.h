// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The worker's stdout: one protocol event per line (Phase 4a design §2.4).

#include "tests/gpu/model/protocol.h"

namespace sb::gpu {
// Writes encode(e) + '\n' to stdout and flushes; thread-safe (a mutex), so the
// stdin thread's protocol errors never interleave with the main thread's lines.
void emitLine(const protocol::Event &e);
// The last line from any thread, then std::_Exit(code), holding the lock, so
// no other thread's line can follow it (the stdin thread's protocol errors).
[[noreturn]] void emitLineAndExit(const protocol::Event &e, int code);
}
