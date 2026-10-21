// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/qt_glue.h"

#include "tests/gpu/worker/worker_window.h"

namespace sb::gpu::glue {

PresentTarget headlessTarget()
{
    PresentTarget t;
    t.headless = true;
    return t;
}

PresentTarget windowlessTarget(Api)
{
    return {};
}

PresentTarget rasterTarget(const WorkerWindow &window)
{
    PresentTarget t;
    const QSize size = window.physicalSize();
    t.windowWidth = std::uint32_t(size.width());
    t.windowHeight = std::uint32_t(size.height());
    return t;
}

} // namespace sb::gpu::glue
