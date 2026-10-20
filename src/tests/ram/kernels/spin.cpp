// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/kernels/spin.h"

#include "tests/cpu/engine/opaque.h"
#include "tests/cpu/kernels/kernel_common.h"

namespace sb::ram {

SB_NOINLINE std::uint64_t spinIterations(std::uint64_t iterations)
{
    std::uint64_t count = 0;
    for (std::uint64_t i = 0; i < iterations; ++i)
        count = cpu::opaqueValue(count + 1);
    return count;
}

} // namespace sb::ram
