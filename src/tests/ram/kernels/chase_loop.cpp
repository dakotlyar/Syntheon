// SPDX-License-Identifier: GPL-3.0-or-later
// The chase loop on its own (Phase 3 design §5.1, §9): its own TU, noinline
// and without IPO, so the optimiser sees neither the cycle nor the caller,
// and each load's address depends on the previous load (nothing to hoist,
// vectorise or overlap).
#include "tests/cpu/kernels/kernel_common.h"
#include "tests/ram/kernels/chase.h"

namespace sb::ram {

SB_NOINLINE const void *chaseHops(const void *start, std::uint64_t hops)
{
    const void *p = start;
    for (std::uint64_t i = 0; i < hops; i += kHopsPerIteration) {
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
        p = *static_cast<const void *const *>(p);
    }
    return p;
}

} // namespace sb::ram
