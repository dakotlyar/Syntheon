// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/engine/time_scale.h"

#include "tests/common/stub_clock.h"

#include <algorithm>
#include <cmath>

namespace sb::cpu {

std::chrono::milliseconds scaledWindow(std::chrono::milliseconds window)
{
    const double ms = std::round(double(window.count()) * tests::stubTimeScale());
    return std::chrono::milliseconds(std::max<long long>(10, (long long)ms));
}

WindowSpec scaled(const WindowSpec &spec)
{
    WindowSpec s = spec;
    s.warmup = scaledWindow(spec.warmup);
    s.rep = scaledWindow(spec.rep);
    return s;
}

} // namespace sb::cpu
