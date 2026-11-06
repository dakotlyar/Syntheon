// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The CPU test's windows at the developer time scale (Phase 2 design §12):
// the Phase 1 stub hook, generalised. SYNTHEON_STUB_SCALE (and SYNTHEON_SMOKE,
// 0.03) shorten every warm-up and repetition by that factor, so CI's smoke run
// does the whole real CPU test in seconds; the numbers are then meaningless.
// Packaged builds compile the hook out: the scale is always 1 there.

#include "tests/cpu/engine/worker_pool.h"

namespace sb::cpu {

// `window` at the current scale, at least 10 ms.
std::chrono::milliseconds scaledWindow(std::chrono::milliseconds window);
WindowSpec scaled(const WindowSpec &spec);

} // namespace sb::cpu
